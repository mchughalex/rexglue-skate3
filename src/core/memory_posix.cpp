/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>

#include <linux/ashmem.h>

// TODO(tomc): Android or maybe na. idk
// #include "xenia/base/main_android.h"
#endif

#if REX_PLATFORM_MAC
#define ftruncate64 ftruncate
#define mmap64 mmap
#endif

namespace rex {
namespace memory {

namespace {

#if REX_PLATFORM_MAC
void AlignHostPageRange(void*& base_address, size_t& length) {
  const uintptr_t page_mask = uintptr_t(page_size() - 1);
  const uintptr_t start = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = start + length;
  const uintptr_t aligned_start = start & ~page_mask;
  const uintptr_t aligned_end = (end + page_mask) & ~page_mask;
  base_address = reinterpret_cast<void*>(aligned_start);
  length = static_cast<size_t>(aligned_end - aligned_start);
}
#endif

}  // namespace

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      // Statically recompiled guest code executes from the host image, never
      // from guest pages, so execute requests are served without PROT_EXEC.
      // This keeps the process W^X: no path through this layer can create
      // executable memory.
      return PROT_READ;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

// ---------------------------------------------------------------------------
// Protection shadow (see RegisterProtectionShadow in the header).
//
// One byte per host page of the registered range: the effective host access
// (0 none, 1 read, 2 read/write - execute requests are served without
// PROT_EXEC, see ToPosixProtectFlags) or kShadowUnknown, which makes
// QueryProtect fall back to /proc/self/maps for that page.
//
// Ordering: a page that gains access is recorded BEFORE the syscall, a page
// that loses access AFTER it. A racing reader (the MMIO handler re-checking
// whether a watch it hit has already been cleared) can therefore only see a
// page as accessible slightly early, in which case it retries the access and
// faults again until the change lands; it never sees a page as protected
// after access has been granted. A failed syscall marks the pages unknown.
constexpr uint8_t kShadowUnknown = 0xFF;

struct ShadowTable {
  uintptr_t begin = 0;  // page-aligned
  uintptr_t end = 0;    // page-aligned, exclusive
  std::unique_ptr<std::atomic<uint8_t>[]> codes;
};
// Published whole, so readers never see a pointer and a range that disagree.
// A replaced table is intentionally leaked: a concurrent reader may still hold it.
std::atomic<ShadowTable*> g_shadow{nullptr};

uint8_t ShadowCode(PageAccess access) {
  const uint32_t prot = ToPosixProtectFlags(access);
  if (prot & PROT_WRITE) {
    return 2;
  }
  return (prot & PROT_READ) ? 1 : 0;
}

PageAccess ShadowAccess(uint8_t code) {
  return code == 2 ? PageAccess::kReadWrite
                   : (code == 1 ? PageAccess::kReadOnly : PageAccess::kNoAccess);
}

// Host page indices [first, last) of the shadow touched by [address, +length).
ShadowTable* ShadowPages(const void* address, size_t length, size_t& first, size_t& last) {
  ShadowTable* shadow = g_shadow.load(std::memory_order_acquire);
  if (!shadow || !length) {
    return nullptr;
  }
  const uintptr_t page_mask = uintptr_t(page_size() - 1);
  uintptr_t begin = reinterpret_cast<uintptr_t>(address) & ~page_mask;
  uintptr_t end = reinterpret_cast<uintptr_t>(address) + length;
  if (end < begin) {
    end = UINTPTR_MAX;
  }
  begin = std::max(begin, shadow->begin);
  end = std::min(end, shadow->end);
  if (begin >= end) {
    return nullptr;
  }
  const size_t page_bytes = page_size();
  first = (begin - shadow->begin) / page_bytes;
  last = (end - shadow->begin + page_bytes - 1) / page_bytes;
  return shadow;
}

void ShadowStore(const void* address, size_t length, uint8_t code) {
  size_t first, last;
  if (ShadowTable* shadow = ShadowPages(address, length, first, last)) {
    for (size_t i = first; i < last; ++i) {
      shadow->codes[i].store(code, std::memory_order_release);
    }
  }
}

// Before a protection change: record pages that are about to become more
// accessible (unknown pages stay unknown).
void ShadowBeginChange(const void* address, size_t length, uint8_t code) {
  size_t first, last;
  if (ShadowTable* shadow = ShadowPages(address, length, first, last)) {
    for (size_t i = first; i < last; ++i) {
      const uint8_t old = shadow->codes[i].load(std::memory_order_relaxed);
      if (old != kShadowUnknown && code > old) {
        shadow->codes[i].store(code, std::memory_order_release);
      }
    }
  }
}

// After a protection change: record the final state, or unknown on failure.
void ShadowEndChange(const void* address, size_t length, uint8_t code, bool succeeded) {
  ShadowStore(address, length, succeeded ? code : kShadowUnknown);
}

bool ShadowQuery(const void* address, PageAccess& access_out, size_t& length_out) {
  size_t first, last;
  ShadowTable* shadow = ShadowPages(address, 1, first, last);
  if (!shadow) {
    return false;
  }
  const uint8_t code = shadow->codes[first].load(std::memory_order_acquire);
  if (code == kShadowUnknown) {
    return false;
  }
  // Length of the run of identically protected pages, like the extent of a
  // /proc/self/maps entry (bounded so the query stays O(1)-ish).
  const size_t total = (shadow->end - shadow->begin) / page_size();
  size_t run_end = first + 1;
  while (run_end < total && run_end - first < 4096 &&
         shadow->codes[run_end].load(std::memory_order_relaxed) == code) {
    ++run_end;
  }
  length_out = run_end * page_size() + shadow->begin - reinterpret_cast<uintptr_t>(address);
  access_out = ShadowAccess(code);
  return true;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

#if REX_PLATFORM_MAC
  if (base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    void* protect_base = base_address;
    size_t protect_length = length;
    AlignHostPageRange(protect_base, protect_length);
    if (mprotect(protect_base, protect_length, static_cast<int>(prot_requested)) == 0) {
      return base_address;
    }
  }
#endif

  // Build flags - always use MAP_FIXED_NOREPLACE for fixed addresses
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

#if REX_PLATFORM_LINUX
  const uint8_t initial_code = prot_initial == PROT_NONE ? 0 : ShadowCode(access);
  if (base_address) {
    ShadowBeginChange(base_address, length, initial_code);
  }
#endif
  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
#if REX_PLATFORM_LINUX
  if (base_address) {
    // EEXIST leaves the existing mapping as it was, so it is not a known state.
    ShadowEndChange(base_address, length, initial_code, result != MAP_FAILED);
  }
#endif
  if (result != MAP_FAILED) {
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      const uint8_t code = ShadowCode(access);
      ShadowBeginChange(base_address, length, code);
      const bool ok = mprotect(base_address, length, static_cast<int>(prot_requested)) == 0;
      ShadowEndChange(base_address, length, code, ok);
      if (ok) {
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
#if REX_PLATFORM_MAC
      AlignHostPageRange(base_address, length);
#endif
      if (mprotect(base_address, length, PROT_NONE) != 0) {
#if REX_PLATFORM_LINUX
        ShadowStore(base_address, length, kShadowUnknown);
#endif
        return false;
      }
#if REX_PLATFORM_LINUX
      ShadowStore(base_address, length, 0);
#endif
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      const bool ok = munmap(base_address, length) == 0;
#if REX_PLATFORM_LINUX
      ShadowStore(base_address, length, ok ? 0 : kShadowUnknown);
#endif
      return ok;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    size_t unused_length = 0;
    if (!ShadowQuery(base_address, *out_old_access, unused_length)) {
      LinuxMapEntry e;
      if (FindEntryForAddress(base_address, e)) {
        *out_old_access = PermsToPageAccess(e.perms);
      }
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
#if REX_PLATFORM_MAC
  AlignHostPageRange(base_address, length);
#endif
#if REX_PLATFORM_LINUX
  const uint8_t code = ShadowCode(access);
  ShadowBeginChange(base_address, length, code);
  const bool ok = mprotect(base_address, length, prot) == 0;
  ShadowEndChange(base_address, length, code, ok);
  return ok;
#else
  return mprotect(base_address, length, prot) == 0;
#endif
}

void RegisterProtectionShadow(void* base, size_t length) {
#if REX_PLATFORM_LINUX
  if (!base || !length) {
    return;
  }
  const size_t page_bytes = page_size();
  const uintptr_t begin = reinterpret_cast<uintptr_t>(base) & ~uintptr_t(page_bytes - 1);
  const uintptr_t end = reinterpret_cast<uintptr_t>(base) + length;
  const size_t pages = (end - begin + page_bytes - 1) / page_bytes;
  auto* shadow = new ShadowTable;
  shadow->begin = begin;
  shadow->end = begin + pages * page_bytes;
  shadow->codes = std::make_unique<std::atomic<uint8_t>[]>(pages);
  // Unmapped holes read as no access, exactly like a failed maps lookup.
  for (size_t i = 0; i < pages; ++i) {
    shadow->codes[i].store(0, std::memory_order_relaxed);
  }
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e) || e.end <= begin || e.start >= end) {
      continue;
    }
    const uintptr_t s = std::max<uintptr_t>(e.start, begin);
    const uintptr_t t = std::min<uintptr_t>(e.end, end);
    const uint8_t code = ShadowCode(PermsToPageAccess(e.perms));
    for (uintptr_t a = s; a < t; a += page_bytes) {
      shadow->codes[(a - begin) / page_bytes].store(code, std::memory_order_relaxed);
    }
  }
  g_shadow.store(shadow, std::memory_order_release);
#else
  static_cast<void>(base);
  static_cast<void>(length);
#endif
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  if (ShadowQuery(base_address, access_out, length)) {
    return true;
  }

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
#if !REX_PLATFORM_ANDROID
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
#if REX_PLATFORM_LINUX
  const uint8_t code = ShadowCode(access);
  if (base_address) {
    ShadowBeginChange(base_address, length, code);
  }
#endif
  void* result = mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                        static_cast<off_t>(file_offset));
#if REX_PLATFORM_LINUX
  if (base_address) {
    ShadowEndChange(base_address, length, code, result == base_address);
  }
#endif
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  const bool ok = munmap(base_address, length) == 0;
#if REX_PLATFORM_LINUX
  ShadowStore(base_address, length, ok ? 0 : kShadowUnknown);
#endif
  return ok;
}

}  // namespace memory
}  // namespace rex
