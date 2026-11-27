#pragma once
// ssasr-nx (2026-11-27, loads batch L107): a switch latched at its first use, without a thread-safe static's guard.
//
// `static const bool on = REXCVAR_GET(x);` in a function is a guarded static: GCC reads its guard byte with an acquire
// load (ldarb) on every call, which on the A57 also keeps the loads after it from starting first. The console census
// (scratchpad global_loads.py, loadsA) had TakeTwinCapture's and LastBindingOf's guards among the game thread's most
// sampled global loads. Here the value is a constant-initialized byte (no guard) read with a plain load: 0 until the
// first call works it out, then 1 (on) or -1 (off). Two threads racing to the first call both work it out and store the
// same value: the switches latched this way are init-only cvars (and an initializer with an effect, TakeTwinCapture's
// hook, sets the same pointer twice). Exact.
#include <atomic>
#include <cstdint>

namespace renut::gpu {

template <typename F>
[[gnu::always_inline]] inline bool LatchedSwitch(std::atomic<int8_t> &slot, F &&compute) {
  int8_t v = slot.load(std::memory_order_relaxed);
  if (v == 0) [[unlikely]] {
    v = compute() ? 1 : -1;
    slot.store(v, std::memory_order_relaxed);
  }
  return v > 0;
}

}  // namespace renut::gpu
