/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <csignal>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#include <rex/dbg.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>

#if REX_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace rex::debug {

bool ReadMemory(const void* address, void* dest, size_t length) {
  if (length == 0)
    return true;
  if (!address || !dest)
    return false;

#if REX_PLATFORM_MAC
  mach_vm_size_t copied = 0;
  return mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(address),
                                length, reinterpret_cast<mach_vm_address_t>(dest),
                                &copied) == KERN_SUCCESS &&
         copied == length;
#elif REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
  iovec local{dest, length};
  iovec remote{const_cast<void*>(address), length};
  const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  return copied >= 0 && size_t(copied) == length;
#else
  return false;
#endif
}

bool IsDebuggerAttached() {
  std::ifstream proc_status_stream("/proc/self/status");
  if (!proc_status_stream.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(proc_status_stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    line_stream >> key;
    if (key == "TracerPid:") {
      uint32_t tracer_pid;
      line_stream >> tracer_pid;
      return tracer_pid != 0;
    }
  }
  return false;
}

void Break() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Install handler for sigtrap only once
    std::signal(SIGTRAP, [](int) {
      // Forward signal to default handler after being caught
      std::signal(SIGTRAP, SIG_DFL);
    });
  });
  std::raise(SIGTRAP);
}

namespace detail {
void DebugPrint(const char* s) {
  std::clog << s << std::endl;
}
}  // namespace detail

}  // namespace rex::debug
