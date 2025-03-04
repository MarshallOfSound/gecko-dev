/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FOGIPC.h"

#include <limits>
#include "mozilla/glean/fog_ffi_generated.h"
#include "mozilla/glean/GleanMetrics.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/gfx/GPUChild.h"
#include "mozilla/gfx/GPUParent.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/Hal.h"
#include "mozilla/MozPromise.h"
#include "mozilla/net/SocketProcessChild.h"
#include "mozilla/net/SocketProcessParent.h"
#include "mozilla/ProcInfo.h"
#include "mozilla/RDDChild.h"
#include "mozilla/RDDParent.h"
#include "mozilla/RDDProcessManager.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/ipc/UtilityProcessManager.h"
#include "mozilla/ipc/UtilityProcessParent.h"
#include "mozilla/Unused.h"
#include "GMPPlatform.h"
#include "GMPServiceParent.h"
#include "nsIXULRuntime.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"

using mozilla::dom::ContentParent;
using mozilla::gfx::GPUChild;
using mozilla::gfx::GPUProcessManager;
using mozilla::ipc::ByteBuf;
using mozilla::ipc::UtilityProcessChild;
using mozilla::ipc::UtilityProcessManager;
using mozilla::ipc::UtilityProcessParent;
using FlushFOGDataPromise = mozilla::dom::ContentParent::FlushFOGDataPromise;

namespace geckoprofiler::markers {

using namespace mozilla;

struct ProcessingTimeMarker {
  static constexpr Span<const char> MarkerTypeName() {
    return MakeStringSpan("ProcessingTime");
  }
  static void StreamJSONMarkerData(baseprofiler::SpliceableJSONWriter& aWriter,
                                   int64_t aDiffMs,
                                   const ProfilerString8View& aType) {
    aWriter.IntProperty("time", aDiffMs);
    aWriter.StringProperty("label", aType);
  }
  static MarkerSchema MarkerTypeDisplay() {
    using MS = MarkerSchema;
    MS schema{MS::Location::MarkerChart, MS::Location::MarkerTable};
    schema.AddKeyLabelFormat("time", "Recorded Time", MS::Format::Milliseconds);
    schema.SetTooltipLabel("{marker.name} - {marker.data.label}");
    schema.SetTableLabel(
        "{marker.name} - {marker.data.label}: {marker.data.time}");
    return schema;
  }
};

}  // namespace geckoprofiler::markers

namespace mozilla::glean {

void RecordPowerMetrics() {
  static uint64_t previousCpuTime = 0, previousGpuTime = 0;

  uint64_t cpuTime, newCpuTime = 0;
  if (NS_SUCCEEDED(GetCpuTimeSinceProcessStartInMs(&cpuTime)) &&
      cpuTime > previousCpuTime) {
    newCpuTime = cpuTime - previousCpuTime;
  }

  uint64_t gpuTime, newGpuTime = 0;
  // Avoid loading gdi32.dll for the Socket process where the GPU is never used.
  if (!XRE_IsSocketProcess() &&
      NS_SUCCEEDED(GetGpuTimeSinceProcessStartInMs(&gpuTime)) &&
      gpuTime > previousGpuTime) {
    newGpuTime = gpuTime - previousGpuTime;
  }

  if (!newCpuTime && !newGpuTime) {
    // Nothing to record.
    return;
  }

  // Compute the process type string.
  nsAutoCString type(XRE_GetProcessTypeString());
  if (XRE_IsContentProcess()) {
    auto* cc = dom::ContentChild::GetSingleton();
    if (cc) {
      type.Assign(dom::RemoteTypePrefix(cc->GetRemoteType()));
      if (StringBeginsWith(type, WEB_REMOTE_TYPE)) {
        type.AssignLiteral("web");
        switch (cc->GetProcessPriority()) {
          case hal::PROCESS_PRIORITY_BACKGROUND:
            type.AppendLiteral(".background");
            break;
          case hal::PROCESS_PRIORITY_FOREGROUND:
            type.AppendLiteral(".foreground");
            break;
          case hal::PROCESS_PRIORITY_BACKGROUND_PERCEIVABLE:
            type.AppendLiteral(".background-perceivable");
            break;
          default:
            break;
        }
      }
    }
  } else if (XRE_IsParentProcess()) {
    if (nsContentUtils::GetUserIsInteracting()) {
      type.AssignLiteral("parent.active");
    } else {
      type.AssignLiteral("parent.inactive");
    }
    hal::WakeLockInformation info;
    GetWakeLockInfo(u"video-playing"_ns, &info);
    if (info.numLocks() != 0 && info.numHidden() < info.numLocks()) {
      type.AppendLiteral(".playing-video");
    } else {
      GetWakeLockInfo(u"audio-playing"_ns, &info);
      if (info.numLocks()) {
        type.AppendLiteral(".playing-audio");
      }
    }
  }

  if (newCpuTime) {
    // The counters are reset at least once a day. Assuming all cores are used
    // continuously, an int32 can hold the data for 24.85 cores.
    // This should be fine for now, but may overflow in the future.
    // Bug 1751277 tracks a newer, bigger counter.
    int32_t nNewCpuTime = int32_t(newCpuTime);
    if (newCpuTime < std::numeric_limits<int32_t>::max()) {
      power::total_cpu_time_ms.Add(nNewCpuTime);
      power::cpu_time_per_process_type_ms.Get(type).Add(nNewCpuTime);
    } else {
      power::cpu_time_bogus_values.Add(1);
    }
    PROFILER_MARKER("CPU Time", OTHER, {}, ProcessingTimeMarker, nNewCpuTime,
                    type);
    previousCpuTime += newCpuTime;
  }

  if (newGpuTime) {
    int32_t nNewGpuTime = int32_t(newGpuTime);
    if (newGpuTime < std::numeric_limits<int32_t>::max()) {
      power::total_gpu_time_ms.Add(nNewGpuTime);
      power::gpu_time_per_process_type_ms.Get(type).Add(nNewGpuTime);
    } else {
      power::gpu_time_bogus_values.Add(1);
    }
    PROFILER_MARKER("GPU Time", OTHER, {}, ProcessingTimeMarker, nNewGpuTime,
                    type);
    previousGpuTime += newGpuTime;
  }
}

/**
 * Flush your data ASAP, either because the parent process is asking you to
 * or because the process is about to shutdown.
 *
 * @param aResolver - The function you need to call with the bincoded,
 *                    serialized payload that the Rust impl hands you.
 */
void FlushFOGData(std::function<void(ipc::ByteBuf&&)>&& aResolver) {
  // Record power metrics right before data is sent to the parent.
  RecordPowerMetrics();

  ByteBuf buf;
  uint32_t ipcBufferSize = impl::fog_serialize_ipc_buf();
  bool ok = buf.Allocate(ipcBufferSize);
  if (!ok) {
    return;
  }
  uint32_t writtenLen = impl::fog_give_ipc_buf(buf.mData, buf.mLen);
  if (writtenLen != ipcBufferSize) {
    return;
  }
  aResolver(std::move(buf));
}

/**
 * Called by FOG on the parent process when it wants to flush all its
 * children's data.
 * @param aResolver - The function that'll be called with the results.
 */
void FlushAllChildData(
    std::function<void(nsTArray<ipc::ByteBuf>&&)>&& aResolver) {
  auto timerId = fog_ipc::flush_durations.Start();

  nsTArray<ContentParent*> parents;
  ContentParent::GetAll(parents);
  nsTArray<RefPtr<FlushFOGDataPromise>> promises;
  for (auto* parent : parents) {
    promises.EmplaceBack(parent->SendFlushFOGData());
  }

  if (GPUProcessManager* gpuManager = GPUProcessManager::Get()) {
    if (GPUChild* gpuChild = gpuManager->GetGPUChild()) {
      promises.EmplaceBack(gpuChild->SendFlushFOGData());
    }
  }

  if (RDDProcessManager* rddManager = RDDProcessManager::Get()) {
    if (RDDChild* rddChild = rddManager->GetRDDChild()) {
      promises.EmplaceBack(rddChild->SendFlushFOGData());
    }
  }

  if (net::SocketProcessParent* socketParent =
          net::SocketProcessParent::GetSingleton()) {
    promises.EmplaceBack(socketParent->SendFlushFOGData());
  }

  {
    RefPtr<mozilla::gmp::GeckoMediaPluginServiceParent> gmps(
        mozilla::gmp::GeckoMediaPluginServiceParent::GetSingleton());
    // There can be multiple Gecko Media Plugin processes, but iterating
    // through them requires locking a mutex and the IPCs need to be sent
    // from a different thread, so it's better to let the
    // GeckoMediaPluginServiceParent code do it for us.
    gmps->SendFlushFOGData(promises);
  }

  if (RefPtr<UtilityProcessManager> utilityManager =
          UtilityProcessManager::GetIfExists()) {
    if (UtilityProcessParent* utilityParent =
            utilityManager->GetProcessParent()) {
      promises.EmplaceBack(utilityParent->SendFlushFOGData());
    }
  }

  if (promises.Length() == 0) {
    // No child processes at the moment. Resolve synchronously.
    fog_ipc::flush_durations.Cancel(std::move(timerId));
    nsTArray<ipc::ByteBuf> results;
    aResolver(std::move(results));
    return;
  }

  // If fog.ipc.flush_failures ever gets too high:
  // TODO: Don't throw away resolved data if some of the promises reject.
  // (not sure how, but it'll mean not using ::All... maybe a custom copy of
  // AllPromiseHolder? Might be impossible outside MozPromise.h)
  FlushFOGDataPromise::All(GetCurrentSerialEventTarget(), promises)
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [aResolver = std::move(aResolver), timerId](
                 FlushFOGDataPromise::AllPromiseType::ResolveOrRejectValue&&
                     aValue) {
               fog_ipc::flush_durations.StopAndAccumulate(std::move(timerId));
               if (aValue.IsResolve()) {
                 aResolver(std::move(aValue.ResolveValue()));
               } else {
                 fog_ipc::flush_failures.Add(1);
                 nsTArray<ipc::ByteBuf> results;
                 aResolver(std::move(results));
               }
             });
}

/**
 * A child process has sent you this buf as a treat.
 * @param buf - a bincoded serialized payload that the Rust impl understands.
 */
void FOGData(ipc::ByteBuf&& buf) {
  fog_ipc::buffer_sizes.Accumulate(buf.mLen);
  impl::fog_use_ipc_buf(buf.mData, buf.mLen);
}

/**
 * Called by FOG on a child process when it wants to send a buf to the parent.
 * @param buf - a bincoded serialized payload that the Rust impl understands.
 */
void SendFOGData(ipc::ByteBuf&& buf) {
  switch (XRE_GetProcessType()) {
    case GeckoProcessType_Content:
      mozilla::dom::ContentChild::GetSingleton()->SendFOGData(std::move(buf));
      break;
    case GeckoProcessType_GMPlugin: {
      mozilla::gmp::SendFOGData(std::move(buf));
    } break;
    case GeckoProcessType_GPU:
      Unused << mozilla::gfx::GPUParent::GetSingleton()->SendFOGData(
          std::move(buf));
      break;
    case GeckoProcessType_RDD:
      Unused << mozilla::RDDParent::GetSingleton()->SendFOGData(std::move(buf));
      break;
    case GeckoProcessType_Socket:
      Unused << net::SocketProcessChild::GetSingleton()->SendFOGData(
          std::move(buf));
      break;
    case GeckoProcessType_Utility:
      Unused << ipc::UtilityProcessChild::GetSingleton()->SendFOGData(
          std::move(buf));
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsuppored process type");
  }
}

/**
 * Called on the parent process to ask all child processes for data,
 * sending it all down into Rust to be used.
 */
RefPtr<GenericPromise> FlushAndUseFOGData() {
  // Record power metrics on the parent before sending requests to child
  // processes.
  RecordPowerMetrics();

  RefPtr<GenericPromise::Private> ret = new GenericPromise::Private(__func__);
  std::function<void(nsTArray<ByteBuf> &&)> resolver =
      [ret](nsTArray<ByteBuf>&& bufs) {
        for (ByteBuf& buf : bufs) {
          FOGData(std::move(buf));
        }
        ret->Resolve(true, __func__);
      };
  FlushAllChildData(std::move(resolver));
  return ret;
}

void TestTriggerMetrics(uint32_t aProcessType,
                        const RefPtr<dom::Promise>& promise) {
  switch (aProcessType) {
    case nsIXULRuntime::PROCESS_TYPE_GMPLUGIN: {
      RefPtr<mozilla::gmp::GeckoMediaPluginServiceParent> gmps(
          mozilla::gmp::GeckoMediaPluginServiceParent::GetSingleton());
      gmps->TestTriggerMetrics()->Then(
          GetCurrentSerialEventTarget(), __func__,
          [promise]() { promise->MaybeResolveWithUndefined(); },
          [promise]() { promise->MaybeRejectWithUndefined(); });
    } break;
    case nsIXULRuntime::PROCESS_TYPE_GPU:
      gfx::GPUProcessManager::Get()->TestTriggerMetrics()->Then(
          GetCurrentSerialEventTarget(), __func__,
          [promise]() { promise->MaybeResolveWithUndefined(); },
          [promise]() { promise->MaybeRejectWithUndefined(); });
      break;
    case nsIXULRuntime::PROCESS_TYPE_RDD:
      RDDProcessManager::Get()->TestTriggerMetrics()->Then(
          GetCurrentSerialEventTarget(), __func__,
          [promise]() { promise->MaybeResolveWithUndefined(); },
          [promise]() { promise->MaybeRejectWithUndefined(); });
      break;
    case nsIXULRuntime::PROCESS_TYPE_SOCKET:
      Unused << net::SocketProcessParent::GetSingleton()
                    ->SendTestTriggerMetrics()
                    ->Then(
                        GetCurrentSerialEventTarget(), __func__,
                        [promise]() { promise->MaybeResolveWithUndefined(); },
                        [promise]() { promise->MaybeRejectWithUndefined(); });
      break;
    case nsIXULRuntime::PROCESS_TYPE_UTILITY:
      Unused << ipc::UtilityProcessManager::GetSingleton()
                    ->GetProcessParent()
                    ->SendTestTriggerMetrics()
                    ->Then(
                        GetCurrentSerialEventTarget(), __func__,
                        [promise]() { promise->MaybeResolveWithUndefined(); },
                        [promise]() { promise->MaybeRejectWithUndefined(); });
      break;
    default:
      promise->MaybeRejectWithUndefined();
      break;
  }
}

}  // namespace mozilla::glean
