/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_VsyncDispatcher_h
#define mozilla_widget_VsyncDispatcher_h

#include "mozilla/DataMutex.h"
#include "mozilla/TimeStamp.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"
#include "mozilla/RefPtr.h"
#include "VsyncSource.h"

namespace mozilla {

class VsyncObserver {
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

 public:
  // The method called when a vsync occurs. Return true if some work was done.
  // In general, this vsync notification will occur on the hardware vsync
  // thread from VsyncSource. But it might also be called on PVsync ipc thread
  // if this notification is cross process. Thus all observer should check the
  // thread model before handling the real task.
  virtual bool NotifyVsync(const VsyncEvent& aVsync) = 0;

 protected:
  VsyncObserver() = default;
  virtual ~VsyncObserver() = default;
};  // VsyncObserver

// Used to dispatch vsync events in the parent process to compositors.
//
// When the compositor is in-process, CompositorWidgets own a
// CompositorVsyncDispatcher, and directly attach the compositor's observer
// to it.
//
// When the compositor is out-of-process, the CompositorWidgetDelegate owns
// the vsync dispatcher instead. The widget receives vsync observer/unobserve
// commands via IPDL, and uses this to attach a CompositorWidgetVsyncObserver.
// This observer forwards vsync notifications (on the vsync thread) to a
// dedicated vsync I/O thread, which then forwards the notification to the
// compositor thread in the compositor process.
class CompositorVsyncDispatcher final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CompositorVsyncDispatcher)

 public:
  CompositorVsyncDispatcher();
  explicit CompositorVsyncDispatcher(RefPtr<gfx::VsyncSource> aVsyncSource);

  // Called on the vsync thread when a hardware vsync occurs
  void NotifyVsync(const VsyncEvent& aVsync);

  void MoveToSource(const RefPtr<gfx::VsyncSource>& aVsyncSource);

  // Compositor vsync observers must be added/removed on the compositor thread
  void SetCompositorVsyncObserver(VsyncObserver* aVsyncObserver);
  void Shutdown();

 private:
  virtual ~CompositorVsyncDispatcher();
  void ObserveVsync(bool aEnable);

  RefPtr<gfx::VsyncSource> mVsyncSource;
  Mutex mCompositorObserverLock;
  RefPtr<VsyncObserver> mCompositorVsyncObserver;
  bool mDidShutdown;
};

// Dispatch vsync events to various observers. This is used by:
//  - Parent process refresh driver timers
//  - IPC for content process refresh driver timers (VsyncParent <->
//  VsyncMainChild)
//  - IPC for content process worker requestAnimationFrame (VsyncParent <->
//  VsyncWorkerChild)
class RefreshTimerVsyncDispatcher final {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(RefreshTimerVsyncDispatcher)

 public:
  explicit RefreshTimerVsyncDispatcher(gfx::VsyncSource* aVsyncSource);

  // Please check CompositorVsyncDispatcher::NotifyVsync().
  void NotifyVsync(const VsyncEvent& aVsync);

  void MoveToSource(gfx::VsyncSource* aVsyncSource);

  // Add a vsync observer to this dispatcher. This is a no-op if the observer is
  // already registered. Can be called from any thread.
  void AddVsyncObserver(VsyncObserver* aVsyncObserver);

  // Remove a vsync observer from this dispatcher. This is a no-op if the
  // observer is not registered. Can be called from any thread.
  void RemoveVsyncObserver(VsyncObserver* aVsyncObserver);

 private:
  virtual ~RefreshTimerVsyncDispatcher();
  void UpdateVsyncStatus();
  bool NeedsVsync();

  // We need to hold a weak ref to the vsync source we belong to in order to
  // notify it of our vsync requirement. The vsync source holds a RefPtr to us,
  // so we can't hold a RefPtr back without causing a cyclic dependency.
  gfx::VsyncSource* mVsyncSource;
  DataMutex<nsTArray<RefPtr<VsyncObserver>>> mVsyncObservers;
};

}  // namespace mozilla

#endif  // mozilla_widget_VsyncDispatcher_h
