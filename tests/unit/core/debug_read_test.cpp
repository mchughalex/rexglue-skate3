#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <memory>

#include <rex/dbg.h>
#include <rex/memory/utils.h>

TEST_CASE("Diagnostic reads tolerate corrupt and protected pointers", "[core][debug]") {
#if REX_PLATFORM_WIN32 || REX_PLATFORM_MAC || (REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID)
  using namespace rex::memory;
  const size_t page = page_size();
  const size_t length = page * 2;
  const auto release = [length](uint8_t* data) {
    DeallocFixed(data, REX_PLATFORM_WIN32 ? 0 : length, DeallocationType::kRelease);
  };
  std::unique_ptr<uint8_t, decltype(release)> memory(
      static_cast<uint8_t*>(
          AllocFixed(nullptr, length, AllocationType::kReserveCommit, PageAccess::kReadWrite)),
      release);
  REQUIRE(memory);

  const std::array<uint32_t, 4> node = {0, 0, 0x4412B31E, 0x40001000};
  std::memcpy(memory.get(), node.data(), sizeof(node));
  std::array<uint32_t, 4> copy{};
  REQUIRE(Protect(memory.get(), page, PageAccess::kReadOnly));
  REQUIRE(rex::debug::ReadMemory(memory.get(), copy.data(), sizeof(copy)));
  CHECK(copy == node);

  // A corrupt node can cross into a guard page. The reader must fail without a fault.
  REQUIRE(Protect(memory.get() + page, page, PageAccess::kNoAccess));
  CHECK_FALSE(rex::debug::ReadMemory(memory.get() + page, copy.data(), sizeof(copy)));
  CHECK_FALSE(rex::debug::ReadMemory(memory.get() + page - 4, copy.data(), sizeof(copy)));
  CHECK_FALSE(rex::debug::ReadMemory(reinterpret_cast<void*>(1), copy.data(), sizeof(copy)));
  CHECK_FALSE(rex::debug::ReadMemory(nullptr, copy.data(), sizeof(copy)));
  CHECK_FALSE(rex::debug::ReadMemory(memory.get(), copy.data(), SIZE_MAX));
  CHECK(rex::debug::ReadMemory(nullptr, nullptr, 0));
#else
  SKIP("Safe diagnostic reads are unavailable on this platform");
#endif
}
