#include <array>
#include <bit>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>

namespace {

using Platform = rex::platform::FPSCRPlatform;
using FPSCR = rex::ppc::FPSCRRegister;

struct RestoreHostFpscr {
  uint32_t value = Platform::getcsr();
  ~RestoreHostFpscr() { Platform::setcsr(value); }
};

uint32_t RoundTripFloat(uint32_t bits) {
  volatile float input = std::bit_cast<float>(bits);
  volatile double extended = static_cast<double>(input);
  volatile float stored = static_cast<float>(extended);
  return std::bit_cast<uint32_t>(static_cast<float>(stored));
}

void CheckScalarRoundTrips() {
  constexpr std::array<uint32_t, 7> values = {0x00000001, 0x007FFFFF, 0x00007041, 0x80000001,
                                              0x807FFFFF, 0x80007041, 0x3F800000};
  for (uint32_t value : values) {
    CAPTURE(value);
    CHECK(RoundTripFloat(value) == value);
  }
}

}  // namespace

TEST_CASE("FPSCR scalar subnormal policy preserves the legacy default", "[ppc][fpscr]") {
  RestoreHostFpscr restore;
  constexpr uint32_t kSubnormal = 0x00007041;

  SECTION("default initialization flushes scalar and VMX subnormals") {
    Platform::setcsr(Platform::getcsr() & ~FPSCR::FlushMask);
    FPSCR fpscr{};
    fpscr.InitHost();

    CHECK((fpscr.fpu_csr & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK((fpscr.vmx_csr & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK(RoundTripFloat(kSubnormal) == 0);

    fpscr.storeFromGuest(rex::ppc::kRoundTowardZero);
    fpscr.loadFromHost();
    fpscr.enableFlushModeUnconditional();
    fpscr.disableFlushModeUnconditional();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK(RoundTripFloat(kSubnormal) == 0);
  }

  SECTION("zero-initialized contexts retain legacy behavior") {
    Platform::setcsr(Platform::getcsr() & ~FPSCR::FlushMask);
    FPSCR fpscr{};
    fpscr.loadFromHost();
    fpscr.disableFlushMode();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK(RoundTripFloat(kSubnormal) == 0);
  }

  SECTION("host thread initialization preserves native mode until a guest callback") {
    Platform::setcsr(Platform::getcsr() | FPSCR::FlushMask);
    FPSCR fpscr{};
    const uint32_t host_csr = fpscr.getcsr();
    fpscr.InitHost(false);
    fpscr.setcsr(host_csr);

    CHECK(Platform::getcsr() == host_csr);
    fpscr.disableFlushMode();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == 0);
    CheckScalarRoundTrips();
  }

  SECTION("host callbacks use the legacy mode without changing native initialization") {
    Platform::setcsr(Platform::getcsr() & ~FPSCR::FlushMask);
    FPSCR fpscr{};
    const uint32_t host_csr = fpscr.getcsr();
    fpscr.InitHost();
    fpscr.setcsr(host_csr);

    CHECK(Platform::getcsr() == host_csr);
    fpscr.disableFlushMode();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK(RoundTripFloat(kSubnormal) == 0);
  }

  SECTION("precise scalar mode survives VMX and FPSCR transitions") {
    Platform::setcsr(Platform::getcsr() | FPSCR::FlushMask);
    FPSCR fpscr{};
    fpscr.InitHost(false);

    CHECK((fpscr.fpu_csr & FPSCR::FlushMask) == 0);
    CHECK((fpscr.vmx_csr & FPSCR::FlushMask) == FPSCR::FlushMask);
    CheckScalarRoundTrips();

    fpscr.enableFlushModeUnconditional();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == FPSCR::FlushMask);
    fpscr.loadFromHost();
    fpscr.disableFlushMode();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == 0);
    CheckScalarRoundTrips();

    fpscr.enableFlushMode();
    fpscr.disableFlushModeUnconditional();
    CHECK((Platform::getcsr() & FPSCR::FlushMask) == 0);
    CheckScalarRoundTrips();

    for (uint32_t rounding : {rex::ppc::kRoundNearest, rex::ppc::kRoundTowardZero,
                              rex::ppc::kRoundUp, rex::ppc::kRoundDown}) {
      fpscr.storeFromGuest(rounding);
      CHECK((fpscr.fpu_csr & FPSCR::FlushMask) == 0);
      CheckScalarRoundTrips();
    }

    Platform::setcsr(Platform::getcsr() | FPSCR::FlushMask);
    fpscr.loadFromHost();
    CHECK((fpscr.fpu_csr & FPSCR::FlushMask) == 0);
    fpscr.disableFlushMode();
    CheckScalarRoundTrips();

    fpscr.InitHost(true);
    CHECK((fpscr.fpu_csr & FPSCR::FlushMask) == FPSCR::FlushMask);
    CHECK(RoundTripFloat(kSubnormal) == 0);
  }
}
