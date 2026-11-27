/**
 * ssasr-nx (2026-11-27, the thread batch F3): a function the kernel calls on a guest thread that is about to block or
 * give up its core (an object wait, a delay, a yield). The renderer sets it to publish draws the thread holds open
 * (render_thread.cpp's draw runs), so that a thread never blocks while its open run holds up the submit worker and the
 * work other threads queued after it. Null: nothing (one relaxed load a wait).
 */
#pragma once

#include <atomic>

namespace rex::kernel {

inline std::atomic<void (*)()> g_guest_block_hook{nullptr};

inline void SetGuestBlockHook(void (*hook)()) {
  g_guest_block_hook.store(hook, std::memory_order_relaxed);
}

[[gnu::always_inline]] inline void RunGuestBlockHook() {
  if (void (*hook)() = g_guest_block_hook.load(std::memory_order_relaxed)) [[unlikely]]
    hook();
}

}  // namespace rex::kernel
