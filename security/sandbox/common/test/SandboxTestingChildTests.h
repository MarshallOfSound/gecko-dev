/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "SandboxTestingChild.h"

#include "mozilla/StaticPrefs_security.h"
#ifdef XP_MACOSX
#  include "nsCocoaFeatures.h"
#endif
#include "nsXULAppAPI.h"

#ifdef XP_UNIX
#  include <fcntl.h>
#  include <netdb.h>
#  ifdef XP_LINUX
#    include <sys/prctl.h>
#    include <sys/ioctl.h>
#    include <termios.h>
#    include <sys/resource.h>
#    include <sys/time.h>
#    include <sys/utsname.h>
#    include <sched.h>
#    include <sys/syscall.h>
#    include <sys/un.h>
#    include "mozilla/ProcInfo_linux.h"
#    ifdef MOZ_X11
#      include "X11/Xlib.h"
#      include "X11UndefineNone.h"
#    endif  // MOZ_X11
#  endif    // XP_LINUX
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
#endif

#ifdef XP_MACOSX
#  include <spawn.h>
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreGraphics/CoreGraphics.h>
namespace ApplicationServices {
#  include <ApplicationServices/ApplicationServices.h>
}
#endif

#ifdef XP_WIN
#  include <stdio.h>
#  include <winternl.h>

#  include "mozilla/DynamicallyLinkedFunctionPtr.h"
#  include "nsAppDirectoryServiceDefs.h"
#endif

constexpr bool kIsDebug =
#ifdef DEBUG
    true;
#else
    false;
#endif

namespace mozilla {

#ifdef XP_LINUX
static void RunTestsSched(SandboxTestingChild* child) {
  struct sched_param param_pid_0 = {};
  child->ErrnoTest("sched_getparam(0)"_ns, true,
                   [&] { return sched_getparam(0, &param_pid_0); });

  struct sched_param param_pid_tid = {};
  child->ErrnoTest("sched_getparam(tid)"_ns, true, [&] {
    return sched_getparam((pid_t)syscall(__NR_gettid), &param_pid_tid);
  });

  struct sched_param param_pid_Ntid = {};
  child->ErrnoValueTest("sched_getparam(Ntid)"_ns, EPERM, [&] {
    return sched_getparam((pid_t)(syscall(__NR_gettid) - 1), &param_pid_Ntid);
  });
}
#endif  // XP_LINUX

#ifdef XP_WIN
/**
 * Uses NtCreateFile directly to test file system brokering.
 *
 */
static void FileTest(const nsCString& aName, const char* aSpecialDirName,
                     const nsString& aRelativeFilePath, ACCESS_MASK aAccess,
                     bool aExpectSuccess, SandboxTestingChild* aChild) {
  static const StaticDynamicallyLinkedFunctionPtr<decltype(&NtCreateFile)>
      pNtCreateFile(L"ntdll.dll", "NtCreateFile");
  static const StaticDynamicallyLinkedFunctionPtr<decltype(&NtClose)> pNtClose(
      L"ntdll.dll", "NtClose");

  // Start the filename with the NT namespace
  nsString testFilename(u"\\??\\"_ns);
  nsString dirPath;
  aChild->SendGetSpecialDirectory(nsDependentCString(aSpecialDirName),
                                  &dirPath);
  testFilename.Append(dirPath);
  testFilename.AppendLiteral("\\");
  testFilename.Append(aRelativeFilePath);

  UNICODE_STRING uniFileName;
  ::RtlInitUnicodeString(&uniFileName, testFilename.get());

  OBJECT_ATTRIBUTES objectAttributes;
  InitializeObjectAttributes(&objectAttributes, &uniFileName,
                             OBJ_CASE_INSENSITIVE, nullptr, nullptr);

  HANDLE fileHandle = INVALID_HANDLE_VALUE;
  IO_STATUS_BLOCK ioStatusBlock = {};

  ULONG createOptions = StringEndsWith(testFilename, u"\\"_ns) ||
                                StringEndsWith(testFilename, u"/"_ns)
                            ? FILE_DIRECTORY_FILE
                            : FILE_NON_DIRECTORY_FILE;
  NTSTATUS status = pNtCreateFile(
      &fileHandle, aAccess, &objectAttributes, &ioStatusBlock, nullptr, 0, 0,
      FILE_OPEN_IF, createOptions | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);

  if (fileHandle != INVALID_HANDLE_VALUE) {
    pNtClose(fileHandle);
  }

  nsCString accessString;
  if ((aAccess & FILE_GENERIC_READ) == FILE_GENERIC_READ) {
    accessString.AppendLiteral("r");
  }
  if ((aAccess & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE) {
    accessString.AppendLiteral("w");
  }
  if ((aAccess & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE) {
    accessString.AppendLiteral("e");
  }

  nsCString msgRelPath = NS_ConvertUTF16toUTF8(aRelativeFilePath);
  for (size_t i = 0, j = 0; i < aRelativeFilePath.Length(); ++i, ++j) {
    if (aRelativeFilePath[i] == u'\\') {
      msgRelPath.Insert('\\', j++);
    }
  }

  nsCString message;
  message.AppendPrintf(
      "Special dir: %s, file: %s, access: %s , returned status: %lx",
      aSpecialDirName, msgRelPath.get(), accessString.get(), status);

  aChild->SendReportTestResults(aName, aExpectSuccess == NT_SUCCESS(status),
                                message);
}
#endif

#ifdef XP_MACOSX
/*
 * Test if this process can launch another process with posix_spawnp,
 * exec, and LSOpenCFURLRef. All launches are expected to fail. In processes
 * where the sandbox permits reading of file metadata (content processes at
 * this time), we expect the posix_spawnp error to be EPERM. In processes
 * without that permission, we expect ENOENT. Changing the sandbox policy
 * may break this assumption, but the important aspect to test for is that the
 * launch is not permitted.
 */
void RunMacTestLaunchProcess(SandboxTestingChild* child,
                             int aPosixSpawnExpectedError = ENOENT) {
  // Test that posix_spawnp fails
  char* argv[2];
  argv[0] = const_cast<char*>("bash");
  argv[1] = NULL;
  int rv = posix_spawnp(NULL, "/bin/bash", NULL, NULL, argv, NULL);
  nsPrintfCString posixSpawnMessage("posix_spawnp returned %d, expected %d", rv,
                                    aPosixSpawnExpectedError);
  child->SendReportTestResults("posix_spawnp test"_ns,
                               rv == aPosixSpawnExpectedError,
                               posixSpawnMessage);

  // Test that exec fails
  child->ErrnoTest("execv /bin/bash test"_ns, false, [&] {
    char* argvp = NULL;
    return execv("/bin/bash", &argvp);
  });

  // Test that launching an application using LSOpenCFURLRef fails
  char* uri;
  if (nsCocoaFeatures::OnCatalinaOrLater()) {
    uri = const_cast<char*>("/System/Applications/Utilities/Console.app");
  } else {
    uri = const_cast<char*>("/Applications/Utilities/Console.app");
  }
  CFStringRef filePath = ::CFStringCreateWithCString(kCFAllocatorDefault, uri,
                                                     kCFStringEncodingUTF8);
  CFURLRef urlRef = ::CFURLCreateWithFileSystemPath(
      kCFAllocatorDefault, filePath, kCFURLPOSIXPathStyle, false);
  if (!urlRef) {
    child->SendReportTestResults("LSOpenCFURLRef"_ns, false,
                                 "CFURLCreateWithFileSystemPath failed"_ns);
    return;
  }

  OSStatus status = ApplicationServices::LSOpenCFURLRef(urlRef, NULL);
  ::CFRelease(urlRef);
  nsPrintfCString lsMessage(
      "LSOpenCFURLRef returned %d, "
      "expected kLSServerCommunicationErr (%d)",
      status, ApplicationServices::kLSServerCommunicationErr);
  child->SendReportTestResults(
      "LSOpenCFURLRef"_ns,
      status == ApplicationServices::kLSServerCommunicationErr, lsMessage);
}

/*
 * Test if this process can connect to the macOS window server.
 * When |aShouldHaveAccess| is true, the test passes if access is __permitted__.
 * When |aShouldHaveAccess| is false, the test passes if access is __blocked__.
 */
void RunMacTestWindowServer(SandboxTestingChild* child,
                            bool aShouldHaveAccess = false) {
  // CGSessionCopyCurrentDictionary() returns NULL when a
  // connection to the window server is not available.
  CFDictionaryRef windowServerDict = CGSessionCopyCurrentDictionary();
  bool gotWindowServerDetails = (windowServerDict != nullptr);
  bool testPassed = (gotWindowServerDetails == aShouldHaveAccess);
  child->SendReportTestResults(
      "CGSessionCopyCurrentDictionary"_ns, testPassed,
      gotWindowServerDetails
          ? "dictionary returned, access is permitted"_ns
          : "no dictionary returned, access appears blocked"_ns);
  if (windowServerDict != nullptr) {
    CFRelease(windowServerDict);
  }
}
#endif /* XP_MACOSX */

void RunTestsContent(SandboxTestingChild* child) {
  MOZ_ASSERT(child, "No SandboxTestingChild*?");

#ifdef XP_UNIX
  struct stat st;
  static const char kAllowedPath[] = "/usr/lib";

  child->ErrnoTest("fstatat_as_stat"_ns, true,
                   [&] { return fstatat(AT_FDCWD, kAllowedPath, &st, 0); });
  child->ErrnoTest("fstatat_as_lstat"_ns, true, [&] {
    return fstatat(AT_FDCWD, kAllowedPath, &st, AT_SYMLINK_NOFOLLOW);
  });

#  ifdef XP_LINUX
  child->ErrnoTest("fstatat_as_fstat"_ns, true,
                   [&] { return fstatat(0, "", &st, AT_EMPTY_PATH); });

  const struct timespec usec = {0, 1000};
  child->ErrnoTest("nanosleep"_ns, true,
                   [&] { return nanosleep(&usec, nullptr); });

  struct timespec res = {0, 0};
  child->ErrnoTest("clock_getres"_ns, true,
                   [&] { return clock_getres(CLOCK_REALTIME, &res); });

  // same process is allowed
  struct timespec tproc = {0, 0};
  clockid_t same_process = MAKE_PROCESS_CPUCLOCK(getpid(), CPUCLOCK_SCHED);
  child->ErrnoTest("clock_gettime_same_process"_ns, true,
                   [&] { return clock_gettime(same_process, &tproc); });

  // different process is blocked by sandbox (SIGSYS, kernel would return
  // EINVAL)
  struct timespec tprocd = {0, 0};
  clockid_t diff_process = MAKE_PROCESS_CPUCLOCK(1, CPUCLOCK_SCHED);
  child->ErrnoValueTest("clock_gettime_diff_process"_ns, ENOSYS,
                        [&] { return clock_gettime(diff_process, &tprocd); });

  // thread is allowed
  struct timespec tthread = {0, 0};
  clockid_t thread =
      MAKE_THREAD_CPUCLOCK((pid_t)syscall(__NR_gettid), CPUCLOCK_SCHED);
  child->ErrnoTest("clock_gettime_thread"_ns, true,
                   [&] { return clock_gettime(thread, &tthread); });

  // getcpu is allowed
  // We're using syscall directly because:
  // - sched_getcpu uses vdso and as a result doesn't go through the sandbox.
  // - getcpu isn't defined in the header files we're using yet.
  int c;
  child->ErrnoTest("getcpu"_ns, true,
                   [&] { return syscall(SYS_getcpu, &c, NULL, NULL); });

  // An abstract socket that does not starts with '/', so we don't want it to
  // work.
  // Checking ENETUNREACH should be thrown by SandboxBrokerClient::Connect()
  // when it detects it does not starts with a '/'
  child->ErrnoValueTest("connect_abstract_blocked"_ns, ENETUNREACH, [&] {
    int sockfd;
    struct sockaddr_un addr;
    char str[] = "\0xyz";  // Abstract socket requires first byte to be NULL
    size_t str_size = 4;

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    memcpy(&addr.sun_path, str, str_size);

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
      return -1;
    }

    int con_st = connect(sockfd, (struct sockaddr*)&addr,
                         sizeof(sa_family_t) + str_size);
    return con_st;
  });

  // An abstract socket that does starts with /, so we do want it to work.
  // Checking ECONNREFUSED because this is what the broker should get
  // when trying to establish the connect call for us if it's allowed;
  // otherwise we get EACCES, meaning that it was passed to the broker
  // (unlike the previous test) but rejected.
  const int errorForX =
      StaticPrefs::security_sandbox_content_headless_AtStartup() ? EACCES
                                                                 : ECONNREFUSED;
  child->ErrnoValueTest("connect_abstract_permit"_ns, errorForX, [&] {
    int sockfd;
    struct sockaddr_un addr;
    // we re-use actual X path, because this is what is allowed within
    // SandboxBrokerPolicyFactory::InitContentPolicy()
    // We can't just use any random path allowed, but one with CONNECT allowed.

    // (Note that the real X11 sockets have names like `X0` for
    // display `:0`; there shouldn't be anything named just `X`.)

    // Abstract socket requires first byte to be NULL
    char str[] = "\0/tmp/.X11-unix/X";
    size_t str_size = 17;

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    memcpy(&addr.sun_path, str, str_size);

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
      return -1;
    }

    int con_st = connect(sockfd, (struct sockaddr*)&addr,
                         sizeof(sa_family_t) + str_size);
    return con_st;
  });

  // Testing FIPS-relevant files, which need to be accessible
  std::vector<std::pair<const char*, bool>> open_tests = {
      {"/dev/random", true}};
  // Not all systems have that file, so we only test access, if it exists
  // in the first place
  if (stat("/proc/sys/crypto/fips_enabled", &st) == 0) {
    open_tests.push_back({"/proc/sys/crypto/fips_enabled", true});
  }

  for (const std::pair<const char*, bool>& to_open : open_tests) {
    child->ErrnoTest("open("_ns + nsCString(to_open.first) + ")"_ns,
                     to_open.second, [&] {
                       int fd = open(to_open.first, O_RDONLY);
                       if (to_open.second && fd > 0) {
                         close(fd);
                       }
                       return fd;
                     });
  }

#    ifdef MOZ_X11
  // Check that X11 access is blocked (bug 1129492).
  // This will fail if security.sandbox.content.headless is turned off.
  if (PR_GetEnv("DISPLAY")) {
    Display* disp = XOpenDisplay(nullptr);

    child->SendReportTestResults(
        "x11_access"_ns, !disp,
        disp ? "XOpenDisplay succeeded"_ns : "XOpenDisplay failed"_ns);
    if (disp) {
      XCloseDisplay(disp);
    }
  }
#    endif  // MOZ_X11
#  endif    // XP_LINUX

#  ifdef XP_MACOSX
  RunMacTestLaunchProcess(child, EPERM);
  RunMacTestWindowServer(child);
#  endif

#elif XP_WIN
  FileTest("read from chrome"_ns, NS_APP_USER_CHROME_DIR, u"sandboxTest.txt"_ns,
           FILE_GENERIC_READ, true, child);
  FileTest("read from profile via relative path"_ns, NS_APP_USER_CHROME_DIR,
           u"..\\sandboxTest.txt"_ns, FILE_GENERIC_READ, false, child);
  // The profile dir is the parent of the chrome dir.
  FileTest("read from chrome using forward slash"_ns,
           NS_APP_USER_PROFILE_50_DIR, u"chrome/sandboxTest.txt"_ns,
           FILE_GENERIC_READ, false, child);

  // Note: these only pass in DEBUG builds because we allow write access to the
  // temp dir for certain test logs and that is where the profile is created.
  FileTest("read from profile"_ns, NS_APP_USER_PROFILE_50_DIR,
           u"sandboxTest.txt"_ns, FILE_GENERIC_READ, kIsDebug, child);
  FileTest("read/write from chrome"_ns, NS_APP_USER_CHROME_DIR,
           u"sandboxTest.txt"_ns, FILE_GENERIC_READ | FILE_GENERIC_WRITE,
           kIsDebug, child);
#else
    child->ReportNoTests();
#endif
}

void RunTestsSocket(SandboxTestingChild* child) {
  MOZ_ASSERT(child, "No SandboxTestingChild*?");

#ifdef XP_UNIX
  child->ErrnoTest("getaddrinfo"_ns, true, [&] {
    struct addrinfo* res;
    int rv = getaddrinfo("localhost", nullptr, nullptr, &res);
    if (res != nullptr) {
      freeaddrinfo(res);
    }
    return rv;
  });

#  ifdef XP_LINUX
  child->ErrnoTest("prctl_allowed"_ns, true, [&] {
    int rv = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    return rv;
  });

  child->ErrnoTest("prctl_blocked"_ns, false, [&] {
    int rv = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    return rv;
  });

  // Testing FIPS-relevant files, which need to be accessible
  std::vector<std::pair<const char*, bool>> open_tests = {
      {"/dev/random", true}};
  // Not all systems have that file, so we only test access, if it exists
  // in the first place
  struct stat st;
  if (stat("/proc/sys/crypto/fips_enabled", &st) == 0) {
    open_tests.push_back({"/proc/sys/crypto/fips_enabled", true});
  }

  for (const std::pair<const char*, bool>& to_open : open_tests) {
    child->ErrnoTest("open("_ns + nsCString(to_open.first) + ")"_ns,
                     to_open.second, [&] {
                       int fd = open(to_open.first, O_RDONLY);
                       if (to_open.second && fd > 0) {
                         close(fd);
                       }
                       return fd;
                     });
  }

  // getcpu is allowed
  // We're using syscall directly because:
  // - sched_getcpu uses vdso and as a result doesn't go through the sandbox.
  // - getcpu isn't defined in the header files we're using yet.
  int c;
  child->ErrnoTest("getcpu"_ns, true,
                   [&] { return syscall(SYS_getcpu, &c, NULL, NULL); });
#  endif  // XP_LINUX
#elif XP_MACOSX
  RunMacTestLaunchProcess(child);
  RunMacTestWindowServer(child);
#else   // XP_UNIX
    child->ReportNoTests();
#endif  // XP_UNIX
}

void RunTestsRDD(SandboxTestingChild* child) {
  MOZ_ASSERT(child, "No SandboxTestingChild*?");

#ifdef XP_UNIX
#  ifdef XP_LINUX
  child->ErrnoValueTest("ioctl_tiocsti"_ns, ENOSYS, [&] {
    int rv = ioctl(1, TIOCSTI, "x");
    return rv;
  });

  struct rusage res = {};
  child->ErrnoTest("getrusage"_ns, true, [&] {
    int rv = getrusage(RUSAGE_SELF, &res);
    return rv;
  });

  child->ErrnoValueTest("unlink"_ns, ENOENT, [&] {
    int rv = unlink("");
    return rv;
  });

  child->ErrnoValueTest("unlinkat"_ns, ENOENT, [&] {
    int rv = unlinkat(AT_FDCWD, "", 0);
    return rv;
  });

  RunTestsSched(child);

  child->ErrnoTest("socket"_ns, false,
                   [] { return socket(AF_UNIX, SOCK_STREAM, 0); });

  child->ErrnoTest("uname"_ns, true, [] {
    struct utsname uts;
    return uname(&uts);
  });

  child->ErrnoValueTest("ioctl_dma_buf"_ns, ENOTTY, [] {
    // Apply the ioctl to the wrong kind of fd; it should fail with
    // ENOTTY (rather than ENOSYS if it were blocked).
    return ioctl(0, _IOW('b', 0, uint64_t), nullptr);
  });

  // getcpu is allowed
  // We're using syscall directly because:
  // - sched_getcpu uses vdso and as a result doesn't go through the sandbox.
  // - getcpu isn't defined in the header files we're using yet.
  int c;
  child->ErrnoTest("getcpu"_ns, true,
                   [&] { return syscall(SYS_getcpu, &c, NULL, NULL); });
#  elif XP_MACOSX
  RunMacTestLaunchProcess(child);
  RunMacTestWindowServer(child);
#  endif
#else  // XP_UNIX
  child->ReportNoTests();
#endif
}

void RunTestsGMPlugin(SandboxTestingChild* child) {
  MOZ_ASSERT(child, "No SandboxTestingChild*?");
#ifdef XP_UNIX
#  ifdef XP_LINUX
  struct utsname utsname_res = {};
  child->ErrnoTest("uname_restricted"_ns, true, [&] {
    int rv = uname(&utsname_res);

    nsCString expectedSysname("Linux"_ns);
    nsCString sysname(utsname_res.sysname);
    nsCString expectedVersion("3"_ns);
    nsCString version(utsname_res.version);
    if ((sysname != expectedSysname) || (version != expectedVersion)) {
      return -1;
    }

    return rv;
  });

  child->ErrnoTest("getuid"_ns, true, [&] { return getuid(); });
  child->ErrnoTest("getgid"_ns, true, [&] { return getgid(); });
  child->ErrnoTest("geteuid"_ns, true, [&] { return geteuid(); });
  child->ErrnoTest("getegid"_ns, true, [&] { return getegid(); });

  RunTestsSched(child);

  std::vector<std::pair<const char*, bool>> open_tests = {
      {"/etc/ld.so.cache", true},
      {"/proc/cpuinfo", true},
      {"/etc/hostname", false}};

  for (const std::pair<const char*, bool>& to_open : open_tests) {
    child->ErrnoTest("open("_ns + nsCString(to_open.first) + ")"_ns,
                     to_open.second, [&] {
                       int fd = open(to_open.first, O_RDONLY);
                       if (to_open.second && fd > 0) {
                         close(fd);
                       }
                       return fd;
                     });
  }
#  elif XP_MACOSX  // XP_LINUX
  RunMacTestLaunchProcess(child);
  /* The Mac GMP process requires access to the window server */
  RunMacTestWindowServer(child, true /* aShouldHaveAccess */);
#  endif           // XP_MACOSX
#else              // XP_UNIX
  child->ReportNoTests();
#endif
}

void RunTestsUtility(SandboxTestingChild* child) {
  MOZ_ASSERT(child, "No SandboxTestingChild*?");

#ifdef XP_UNIX
#  ifdef XP_LINUX
  child->ErrnoValueTest("ioctl_tiocsti"_ns, ENOSYS, [&] {
    int rv = ioctl(1, TIOCSTI, "x");
    return rv;
  });

  struct rusage res;
  child->ErrnoTest("getrusage"_ns, true, [&] {
    int rv = getrusage(RUSAGE_SELF, &res);
    return rv;
  });
#  elif XP_MACOSX  // XP_LINUX
  RunMacTestLaunchProcess(child);
  RunMacTestWindowServer(child);
#  endif           // XP_MACOSX
#elif XP_WIN       // XP_UNIX
  child->ErrnoValueTest("write_only"_ns, EACCES, [&] {
    FILE* rv = fopen("test_sandbox.txt", "w");
    if (rv != nullptr) {
      fclose(rv);
      return 0;
    }
    return -1;
  });
#else              // XP_UNIX
    child->ReportNoTests();
#endif             // XP_MACOSX
}

}  // namespace mozilla
