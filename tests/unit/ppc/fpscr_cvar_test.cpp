#include <atomic>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

REXCVAR_DECLARE(bool, ppc_scalar_flush_denormals);

namespace {

struct FpscrSnapshot {
  std::atomic<bool> captured{false};
  std::atomic<uint32_t> scalar{0};
  std::atomic<uint32_t> vector{0};
  std::atomic<uint32_t> initial{0};
  std::atomic<uint32_t> after_scalar{0};
  std::atomic<uint32_t> after_vector{0};
  std::atomic<uint32_t> after_restore{0};

  void Capture(PPCFPSCRRegister& fpscr) {
    scalar = fpscr.fpu_csr;
    vector = fpscr.vmx_csr;
    initial = fpscr.getcsr();
    fpscr.disableFlushMode();
    after_scalar = fpscr.getcsr();
    fpscr.enableFlushMode();
    after_vector = fpscr.getcsr();
    fpscr.disableFlushMode();
    after_restore = fpscr.getcsr();
    captured = true;
  }
};

class ProbeGuestThread final : public rex::system::XThread {
 public:
  ProbeGuestThread(rex::system::KernelState* kernel, FpscrSnapshot& snapshot)
      : XThread(kernel, 128 * 1024, 0, 0, 0, 0, true), snapshot_(snapshot) {}

  void Execute() override {
    snapshot_.Capture(thread_state()->context()->fpscr);
    Exit(0);
  }

 private:
  FpscrSnapshot& snapshot_;
};

void CheckPreciseSnapshot(const FpscrSnapshot& snapshot) {
  constexpr auto flush = PPCFPSCRRegister::FlushMask;
  REQUIRE(snapshot.captured.load());
  CHECK((snapshot.scalar.load() & flush) == 0);
  CHECK((snapshot.vector.load() & flush) == flush);
  CHECK((snapshot.after_scalar.load() & flush) == 0);
  CHECK((snapshot.after_vector.load() & flush) == flush);
  CHECK((snapshot.after_restore.load() & flush) == 0);
}

}  // namespace

TEST_CASE("FPSCR scalar subnormal setting is configurable", "[ppc][fpscr]") {
  using rex::X_STATUS;

  const auto* flag = rex::cvar::GetFlagInfo("ppc_scalar_flush_denormals");
  REQUIRE(flag != nullptr);
  CHECK(flag->default_value == "true");
  CHECK(flag->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(REXCVAR_GET(ppc_scalar_flush_denormals));

  char program[] = "fpscr_cvar_test";
  char option[] = "--ppc_scalar_flush_denormals=false";
  char* argv[] = {program, option};
  const auto remaining = rex::cvar::Init(2, argv);

  CHECK(remaining.empty());
  CHECK_FALSE(REXCVAR_GET(ppc_scalar_flush_denormals));
  CHECK(rex::cvar::GetFlagByName("ppc_scalar_flush_denormals") == "false");

  rex::Runtime runtime({});
  rex::RuntimeConfig config;
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  FpscrSnapshot guest_snapshot;
  auto guest = rex::system::object_ref<ProbeGuestThread>(
      new ProbeGuestThread(runtime.kernel_state(), guest_snapshot));
  REQUIRE(guest->Create() == X_STATUS_SUCCESS);
  REQUIRE(guest->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  CheckPreciseSnapshot(guest_snapshot);
  CHECK((guest_snapshot.initial.load() & PPCFPSCRRegister::FlushMask) == 0);

  REXCVAR_SET(ppc_scalar_flush_denormals, true);
  FpscrSnapshot host_snapshot;
  auto host = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(runtime.kernel_state(), 128 * 1024, 0, [&] {
        host_snapshot.Capture(rex::runtime::ThreadState::Get()->context()->fpscr);
        return 0;
      }));
  REQUIRE(host->Create() == X_STATUS_SUCCESS);
  REQUIRE(host->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  CheckPreciseSnapshot(host_snapshot);
}
