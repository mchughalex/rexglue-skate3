#include <rex/kernel/guest_presence.h>

#include <atomic>

namespace rex::kernel::guest_presence {
namespace {

// Not 0 before the title has set the context: 0 means "frontend/loading" to
// the consumers, and the native renderer evaluates it from its first frame.
// When that frame came before the title's boot-time XUserSetContext(0x8001, 7)
// (an intermittent startup race), the renderer entered its native loading
// screen state at 0 and then flipped to native gameplay rendering at 7, in the
// middle of the intro movie, with nothing to draw: the boot stalled with no
// frames presented (seen in the stock build too). An unset value compares like
// the boot value 7 (!= 0 and != 1), so every boot now takes the path the
// winning side of the race always took.
constexpr uint32_t kContextUnset = 0xFFFFFFFFu;
std::atomic<uint32_t> g_gameplay_context_value{kContextUnset};

}  // namespace

void NotifyContext(uint32_t user_index, uint32_t context_id, uint32_t value) {
  constexpr uint32_t kGameplayContext = 0x8001;
  if (user_index != 0 || context_id != kGameplayContext) {
    return;
  }
  g_gameplay_context_value.store(value, std::memory_order_relaxed);
}

uint32_t GameplayContextValue() {
  return g_gameplay_context_value.load(std::memory_order_relaxed);
}

}  // namespace rex::kernel::guest_presence
