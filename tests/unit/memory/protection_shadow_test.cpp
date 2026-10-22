/**
 * Unit tests for the POSIX page-protection record (RegisterProtectionShadow).
 *
 * QueryProtect answers from an in-process record of host page protection for
 * a registered range instead of parsing /proc/self/maps. These tests apply
 * random protection changes through the rex::memory API and check every page
 * against an independent /proc/self/maps parse.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>
#include <rex/platform.h>

#if REX_PLATFORM_LINUX

#include <sys/mman.h>
#include <unistd.h>

using namespace rex::memory;

namespace {

// 0 = no access (or unmapped), 1 = read only, 2 = read/write.
int MapsAccess(uintptr_t address) {
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long start = 0, end = 0;
    char perms[5] = {};
    if (std::sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3) {
      continue;
    }
    if (address >= start && address < end) {
      return perms[1] == 'w' ? 2 : (perms[0] == 'r' ? 1 : 0);
    }
  }
  return 0;
}

int AccessCode(PageAccess access) {
  return access == PageAccess::kReadWrite ? 2 : (access == PageAccess::kReadOnly ? 1 : 0);
}

int QueryAccess(uintptr_t address) {
  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  QueryProtect(reinterpret_cast<void*>(address), length, access);
  return AccessCode(access);
}

}  // namespace

TEST_CASE("QueryProtect matches /proc/self/maps for a registered range", "[memory][protection]") {
  const size_t page = page_size();
  const size_t pages = 128;
  const size_t usable = pages - 8;  // the last 8 pages stay an unmapped hole
  auto* base = static_cast<uint8_t*>(
      mmap(nullptr, pages * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(base != MAP_FAILED);
  REQUIRE(munmap(base + usable * page, (pages - usable) * page) == 0);
  // State that exists before registration must be picked up from the kernel.
  REQUIRE(mprotect(base, 4 * page, PROT_READ | PROT_WRITE) == 0);

  RegisterProtectionShadow(base, pages * page);

  const std::string name = "rex_protection_shadow_test_" + std::to_string(getpid());
  FileMappingHandle file = CreateFileMappingHandle(name, 32 * page, PageAccess::kReadWrite, false);
  REQUIRE(file != kFileMappingHandleInvalid);

  const PageAccess kAccesses[] = {PageAccess::kNoAccess, PageAccess::kReadOnly,
                                  PageAccess::kReadWrite, PageAccess::kExecuteReadOnly,
                                  PageAccess::kExecuteReadWrite};
  std::mt19937 rng(1234);
  int mismatches = 0;
  for (int step = 0; step < 800; ++step) {
    const size_t first = rng() % usable;
    const size_t count = std::min<size_t>(1 + rng() % 16, usable - first);
    void* address = base + first * page;
    const size_t length = count * page;
    switch (rng() % 6) {
      case 0:
        // Unaligned lengths round up to whole pages, like mprotect.
        Protect(address, length - (rng() % 2), kAccesses[rng() % 5], nullptr);
        break;
      case 1: {
        PageAccess old_access;
        Protect(address, length, kAccesses[rng() % 5], &old_access);
        break;
      }
      case 2:
        DeallocFixed(address, length, DeallocationType::kDecommit);
        break;
      case 3:
        AllocFixed(address, length, AllocationType::kCommit, kAccesses[rng() % 5]);
        break;
      case 4:
        MapFileView(file, address, length, kAccesses[1 + rng() % 2], 0);
        break;
      case 5:
        if (MapFileView(file, address, length, PageAccess::kReadWrite, 0)) {
          UnmapFileView(file, address, length);
        }
        break;
    }
    if (step % 40 == 0 || step == 799) {
      for (size_t i = 0; i < pages; ++i) {
        const uintptr_t probe = reinterpret_cast<uintptr_t>(base + i * page) + rng() % page;
        if (QueryAccess(probe) != MapsAccess(probe)) {
          ++mismatches;
        }
      }
    }
  }
  CHECK(mismatches == 0);

  CloseFileMappingHandle(file, name);
  munmap(base, usable * page);
}

#endif  // REX_PLATFORM_LINUX
