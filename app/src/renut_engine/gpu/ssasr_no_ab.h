// ssasr-nx (2026-11-26, batch 4 #41; CMake SSASR_NO_AB): a build with no in-run A/B. The A/B word every "the A/B half
// has this part off" test reads is the constant 0 (nothing off, as a run with banjo_renut_ab off has it), so the tests
// fold away (96 in the renderer, the copies' and the natives' own). Included in place of the word's declarations by
// renut_engine/gpu/banjo_renut_tuning.h, cpu/banjo_hot_guest.h and cpu/hot/banjo_hand_natives.h.
#pragma once
#include <atomic>

namespace banjo::renut_gpu {
struct NoAbWord {
  constexpr int load(std::memory_order = std::memory_order_seq_cst) const { return 0; }
};
inline constexpr NoAbWord g_ab_off{};
inline constexpr int g_ab_off_plain = 0;
}  // namespace banjo::renut_gpu
