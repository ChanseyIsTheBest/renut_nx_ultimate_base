/**
 * @file    gpu/backend/render_thread.cpp
 */
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/ssasr_host50.h"  // ssasr-nx (2026-11-26, batch 4)
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)
#include "renut_engine/gpu/sasrt_gpu_round.h"  // sasrt-nx (2026-11-20): the second render-thread round

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <string>
#include <rex/frame_latency.h>
#include <vector>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/display_sync.h"  // sa2-nx (2026-10-28): LateInputGuardUs

#include <rex/cvar.h>
#include <rex/kernel/guest_block_hook.h>  // ssasr-nx (the thread batch F3)

// banjo-nx (2026-10-20, from sa2-nx): see g_done_waiters and Run.
// rr6-nx (2026-11-03, from sasrt-nx, which found this on its console): the game running ahead of the render thread can
// rewrite data before the render thread reads it. Ryujinx rarely shows it; a slow GPU shows it constantly.
REXCVAR_DEFINE_INT32(rr6_frames_ahead, 1, "Banjo/Enhancements",
                     "How many frames the game may run ahead of Renut's render thread. 0: the game waits at its Swap "
                     "until the render thread has done the frame. 1: one frame ahead. 2 (blur-nx #37, near-exact): two "
                     "frames ahead (the game may rewrite data the render thread has yet to read)")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(rr6_diag_render_delay_ms, 0, "Banjo/Enhancements",
                     "Diagnostic: each frame's work on the render thread waits this long first, so the PC lags behind "
                     "the game as a console does")
    .range(0, 500)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_done_notify_waiters, true, "Banjo/Enhancements",
                    "Renut's render thread signals its work done (frames, fences, drains) only when a thread waits for "
                    "it: a signal is a kernel call on the Switch even with nobody waiting, and it made one after every "
                    "batch of work. Off: after every batch, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_render_address_wait, true, "Banjo/Renut",
                    "blur-nx (the Renut audit's #11, exact; the Switch): with the lock-free ring, the render thread sleeps "
                    "on an address (svcWaitForAddress) and a push wakes it with svcSignalToAddress, without g_mutex on "
                    "either side (Blur's Game Master: 0.88 % in notify_one waking it). false: g_work_cv, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_done_lockfree, true, "Banjo/Renut",
                    "blur-nx (the Renut audit's #12, exact): the lock-free ring's batch end counts its items and reads "
                    "the done waiters as atomics, taking g_mutex only to wake one (with banjo_renut_done_notify_waiters). "
                    "false: g_mutex at every batch end, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_prefetch_next, true, "Banjo/Enhancements",
                    "Renut's render thread asks for the next draw's capture (written on the game thread's core) to be "
                    "brought into its cache while it does the draw before it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-20, render-thread batch, experiment #58): see RunRing.
REXCVAR_DEFINE_INT32(banjo_renut_ring_spin_us, 0, "Banjo/Renut",
                     "Experiment: microseconds Renut's render thread looks for new work before it sleeps on an empty "
                     "ring (0: sleeps at once)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_wake_batch, 16, "Banjo/Enhancements",
                     "Renut's render thread, when it has caught up and sleeps, is woken by the game's draws only once "
                     "this many are queued (each wake is a kernel call on both threads; the fourth console run woke it "
                     "for almost every draw). Every other item (frames, fences, resources), every wait for it and a "
                     "2 ms timer still wake it at once. 1 or less: every draw wakes it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_batch_clock, true, "Banjo/Enhancements",
                    "Renut's render thread reads the clock once per batch of work instead of once per item (about "
                    "1,800 items a frame in City Escape); its busy time per frame is the same sum")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// dishonored-nx (sa2-nx's of 2026-10-28; port 105): the render queue as a lock-free ring.
REXCVAR_DEFINE_BOOL(dishonored_renut_lockfree_queue, true, "Banjo/Enhancements",
                    "From dishonored-nx (sa2-nx's): Renut's render thread takes its work from a lock-free ring (one "
                    "compare-and-swap a push) instead of a vector under the queue's mutex, which every draw of the game "
                    "thread took and the render thread took back (two lock round trips a draw on two cores, and the "
                    "wake decision under the lock). Same order, same wakes. Off: the mutex and the vector")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ssasr-nx (2026-11-27, the thread batch T1 / T6 / T7: out/thread_plan.md; the user: "I am ok moving them to another
// thread as long as it as not the render thread").
REXCVAR_DEFINE_BOOL(ssasr_submit_worker, false, "Banjo/Renut",
                    "Every push from a thread other than the render thread goes to a submit ring, in the order of the "
                    "pushes, and a submit worker thread (not the render thread) hands it on to the render thread's "
                    "ring, finishing a staged draw capture first (queued.cpp: the shadow compare, the delta and the "
                    "shadow's upkeep), so the game thread only copies the chunks a draw marked. Exact: the same captures "
                    "in the same order. Needs dishonored_renut_lockfree_queue")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ssasr_setter_log, false, "Banjo/Renut",
                    "With ssasr_submit_worker: the drawing device's setter writes are logged, and the submit worker "
                    "rebuilds the chunks only the setters wrote from its shadow and the log instead of the game thread "
                    "copying them from the device at the draw (the thread batch F1). Exact; checked at the sparse mark "
                    "check (a difference turns it off for the run)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ssasr_draw_runs, false, "Banjo/Renut",
                    "With ssasr_submit_worker: the drawing thread's staged draws go to the submit ring in runs of up to "
                    "six, one slot each, claimed at the run's first draw (its place in the order) and published when "
                    "full, before any other push, before a wait for the render thread and before the thread blocks in "
                    "the kernel (the thread batch F3). The same items in the same order. renut-ultimate (2026-11-27): Beautiful Katamari's PC renderer presented no frame with it on (the submit worker and the setter log alone ran right): checked in ssasr-nx only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ssasr_submit_worker_core, 2, "Banjo/Renut",
                     "The submit worker's core (ssasr_submit_worker; -1: not pinned). The render thread is on core 1 "
                     "(banjo_ring_core) and the game thread on core 0")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__SWITCH__)
// blur-nx (2026-11-17, the Renut audit's #11): the SDK's address-wait helpers (threading_switch.cpp).
extern "C" bool RexSwitchWaitIfEqual(volatile uint32_t* word, uint32_t value, int64_t ns);
extern "C" void RexSwitchWakeAddress(volatile uint32_t* word, int32_t count);
#endif

namespace renut::gpu {
void SettleExportsForFence();  // port 170: render/draw.cpp (banjo_fence_waits_exports)
}

namespace renut::gpu::render_thread {

namespace {

using Clock = std::chrono::steady_clock;

// ssasr-nx (2026-11-27, loads batch L123): the closure behind a pointer (a std::function item is rare: the race's pushes
// are 100 % plain), so that an item is 48 bytes and a ring slot with its sequence one cache line (it was 72 and two):
// half the lines a push writes and the render thread reads.
using Work = std::unique_ptr<std::function<void()>>;
inline Work MakeWork(std::function<void()> &&f) {
  return f ? std::make_unique<std::function<void()>>(std::move(f)) : Work();
}
struct Item {
  Work work;
  bool locked = true;  // run with State().mutex held
  // ssasr-nx (2026-11-27, the thread batch T1): in the submit ring, the item is a draw (the render ring's wake batch);
  // in the padding after 'locked' (an item stays 48 bytes).
  bool submit_draw = false;
  const void *prefetch[2] = {};  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): fetched while the item before runs
  // ssasr-nx (2026-11-26, batch 4 #1): a plain item instead (QueuePrefetchedPlain: the game's draws): a function and
  // its argument, no std::function to build, move into the ring, call through its invoker and destroy.
  void (*plain)(void *) = nullptr;
  void *arg = nullptr;
};

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): the next item's data into the cache (its first lines).
inline void PrefetchItem(const Item &item) {
  if (const auto *a = static_cast<const char *>(item.prefetch[0])) {
    __builtin_prefetch(a);
    __builtin_prefetch(a + 64);
    // sasrt-nx (2026-11-12, batch C/D, item C8; port 93): prefetch[0] is a draw's capture (QueuePrefetched's only
    // caller, QueueCapture): its call's second line (with its binding) and its stack headers' count (blur-nx's, port 76),
    // which the lambda reads first. (sasrt-nx's +320 and +384, its recorded surfaces' headers, are the stack headers'
    // own here, read only when the count is not 0: not fetched.)
    __builtin_prefetch(a + 128);  // (and the stack headers' count: on this line since loads batch L122, not at +256)
  }
  if (const auto *b = static_cast<const char *>(item.prefetch[1])) {
    __builtin_prefetch(b);
    __builtin_prefetch(b + 64);
    __builtin_prefetch(b + 128);
    __builtin_prefetch(b + 192);
    // sasrt-nx (2026-11-20, sasrt_rt2_ring_prefetch; the second render-thread round's #30): a narrowed delta averages
    // ~360 bytes in Sonic Transformed's 4-player race (6-7 lines): two lines more.
    if (sasrt::gpu_round::On(sasrt::gpu_round::kRt2RingPrefetch)) {
      __builtin_prefetch(b + 256);
      __builtin_prefetch(b + 320);
    }
  }
}

std::mutex g_mutex;
std::condition_variable g_work_cv;  // work queued, or stopping
// banjo-nx (banjo_renut_queue_notify_idle): the render thread is in g_work_cv.wait (under g_mutex). A push notifies only
// then: on the Switch every notify is a kernel call, about 1,500 a frame from the game threads, most of them while the
// render thread is busy and not waiting. The render thread reads the queue under g_mutex before it sleeps, so a push
// it has not seen is never missed.
bool g_render_waiting = false;
std::condition_variable g_done_cv;  // work done: frames, fences, drains; or stopped
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_done_notify_waiters): threads in g_done_cv's waits (under g_mutex). A done signal goes
// only when there is one: a waiter checks its condition under g_mutex before it counts itself and sleeps, and the
// state it waits for changes under g_mutex too, so no signal it needs is skipped.
// blur-nx (2026-11-17, the Renut audit's #12): atomics (sequentially consistent), so the ring's batch end can count
// its items and read the waiters without g_mutex (blur_renut_done_lockfree, RunRing); every other access is still
// under g_mutex as before.
std::atomic<int> g_done_waiters{0};
bool DoneNotifyAll() {
  static const bool on = REXCVAR_GET(banjo_renut_done_notify_waiters);
  return !on;
}
// blur-nx (2026-11-17, the Renut audit's #11, blur_renut_render_address_wait; the Switch): the ring's sleep on an
// address instead of g_work_cv. The render thread reads g_wake_word, looks at the ring once more and sleeps while the
// word is unchanged (svcWaitForAddress); a push that publishes, then reads g_waiting true, adds to the word and wakes
// the address (svcSignalToAddress): no g_mutex on either side, and the woken thread does not take a mutex first (a
// condition variable's wake on Horizon hands it the mutex). Either the push's add comes before the render thread's read
// of the word (then its look at the ring, after that read, sees the item) or after it (then the wait sees the word
// changed, or is woken). g_stop_flag: g_stopping for the sleeper, which holds no g_mutex.
std::atomic<uint32_t> g_wake_word{0};
std::atomic<bool> g_stop_flag{false};
// blur-nx (2026-11-17, audit #11): blur_renut_render_address_wait (the Switch only).
bool AddressWakeOn() {
#if defined(__SWITCH__)
  static const bool on = REXCVAR_GET(blur_renut_render_address_wait);
  return on;
#else
  return false;
#endif
}
// A push's wake of the sleeping render thread (g_waiting read true): the address, or g_work_cv (taking g_mutex first,
// so the notify cannot come between the render thread's last look and its wait; Locked: the caller holds g_mutex).
void WakeRenderAddress() {
#if defined(__SWITCH__)
  g_wake_word.fetch_add(1, std::memory_order_seq_cst);
  RexSwitchWakeAddress(reinterpret_cast<volatile uint32_t *>(&g_wake_word), 1);
#endif
}
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): the draws queued before a sleeping render thread is woken (1: each).
size_t WakeBatch() {
  static const size_t n = size_t(std::max(1, int(REXCVAR_GET(banjo_renut_wake_batch))));
  return n;
}
std::vector<Item> g_queue;

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): the queue as a bounded ring of
// kRingSlots items (D. Vyukov's bounded MPMC queue, with the render thread its one consumer). A push claims the next
// position with a compare-and-swap, moves its item into the slot and publishes it by the slot's sequence (release); the
// render thread takes the positions in order (acquire), runs each item in its slot and gives the slot back for the push
// kRingSlots positions later. The order is the order of the claims, as it was the order of the lock. sa2-nx's 2P console
// profile had the queue's mutex and vector at 1.4 % of the game thread and its share of the locks (2.5 %) besides, with
// the render thread on the same lock's cache line at every batch. A full ring (kRingSlots items queued, more than the
// game thread can be ahead by with one frame queued) makes a pusher wait for the render thread; the render thread itself
// runs its own item at once instead (logged: never expected).
// Latched by Start before the thread is made and before g_started's release, which every Push reads first (acquire): a
// plain load per push.
// ssasr-nx (2026-11-27, batch 5 N17): the three latched words every push reads on a cache line of their own (the console's
// 5C profile: 54 samples, 1.1 % of the game thread, on the load of g_notify_idle after the publish; its line held
// video.cpp's .data too).
struct alignas(64) PushConfig {
  bool lockfree = false;
  bool notify_idle = true;
  bool submit = false;  // ssasr-nx (2026-11-27, the thread batch T1): the submit ring takes the pushes (Start latches it)
  size_t wake_batch = 16;
};
PushConfig g_push_config;
bool &g_lockfree = g_push_config.lockfree;
bool LockfreeOn() {
  return g_lockfree;
}
// The same for the two PushRing reads at every push: QueueNotifyIdleOn and WakeBatch, latched by Start.
bool &g_notify_idle = g_push_config.notify_idle;
size_t &g_wake_batch = g_push_config.wake_batch;
// sa2-nx (2026-10-28, the console's first run of the ring): with 32,768 slots (2 MB) every push's slot was a line from
// memory (a slot came round again nine frames later, long out of the caches): 30 of PushRing's 86 samples were the load
// of its sequence. A push now prefetches the slot kRingAhead positions on for writing, a few draws before it needs it,
// and the ring is 16,384 slots (1 MB): still more than two frames of the two-player race's items (about 3,500 a frame),
// so that a pusher never waits on a full ring while it may hold a lock the render thread's item needs.
// ssasr-nx (2026-11-27, loads batch L94): 8,192 one-line slots (512 KB; it was 16,384 of two lines, 2 MB, the size of
// the cores' shared L2, a slot coming round again eight frames later, long gone from it): still more than three frames
// of this race's items (~2,400 a frame at its start).
constexpr uint64_t kRingSlots = uint64_t(1) << 14;  // renut-ultimate: the base's 16,384 kept (ssasr-nx's L94 took it to 8,192: a game with more items a frame than its race fills it sooner)
constexpr uint64_t kRingAhead = 8;
// ssasr-nx (2026-11-27, batch 5 N20): the publish as dmb ishst and a plain store (AArch64 only; elsewhere the release).
#if defined(__aarch64__)
constexpr bool kStoreBarrierPublish = true;
#else
constexpr bool kStoreBarrierPublish = false;
#endif
struct alignas(64) RingSlot {
  std::atomic<uint64_t> seq{0};
  Item item;
};
static_assert(sizeof(RingSlot) == 64, "a ring slot is one cache line (L123)");
RingSlot *g_ring = nullptr;                      // made by Start, before the thread runs
// ssasr-nx (2026-11-27, batch 5 N4): with the pushers' first position whose draw may need a wake (PushRingWith) on the
// same line, which only the pushers write (a thread_local was a call at every push on the Switch: __aarch64_read_tp).
// Any pusher's look holds for every position (it is about the render thread's state), so one word serves them all.
struct alignas(64) EnqueueLine {
  std::atomic<uint64_t> pos{0};
  std::atomic<uint64_t> next_wake_check{0};
};
EnqueueLine g_enqueue_line;
std::atomic<uint64_t> &g_enqueue = g_enqueue_line.pos;  // the next position a push claims (the items queued)
alignas(64) std::atomic<uint64_t> g_dequeue{0};  // the next position the render thread takes (its own)
// ssasr-nx (2026-11-27, batch 5 N7): g_waiting on a cache line of its own. The render thread writes it at every sleep
// and wake; the linker had put it on the line of g_started and g_accepting, which every push reads.
struct alignas(64) WaitingLine {
  std::atomic<bool> flag{false};
};
WaitingLine g_waiting_line;
std::atomic<bool> &g_waiting = g_waiting_line.flag;  // the render thread is going to sleep, or asleep, in g_work_cv
std::atomic<bool> g_accepting{false};            // started and not stopping: pushes go to the ring
// The slot at 'pos' holds a published item.
inline bool RingReady(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_acquire) == pos + 1;
}
// The same as the sleeping side's last look: sequentially consistent, so that it is ordered after its store of
// g_waiting (an ldar after an stlr on AArch64, which needs no barrier between them; a push publishes the same way).
inline bool RingReadySc(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_seq_cst) == pos + 1;
}
std::once_flag g_start_once;
// bangaio-nx (2026-10-29, v6): set once Start's call_once has run (relaxed: Push then takes g_mutex, under which Start
// set g_running). Push checks it first: std::call_once was a pthread_once call at every queued draw on the game thread
// (0.6 % of it in the v4 console storm).
std::atomic<bool> g_started{false};
std::thread::id g_thread_id;
bool g_running = false;   // the thread takes work
bool g_stopping = false;  // Stop: finish what is queued, then end
uint64_t g_items_queued = 0;
std::atomic<uint64_t> g_items_done{0};  // blur-nx (2026-11-17, audit #12): see g_done_waiters
uint64_t g_frames_queued = 0;
uint64_t g_frames_done = 0;
uint64_t g_fences_done = 0;  // fence completions written, plus callbacks reached
uint64_t g_callbacks_reached = 0;
uint64_t g_callbacks_run = 0;
// Fence completions waiting for their callbacks, in order.
struct PendingFence {
  u32 completion_va;
  u32 fence;
  uint64_t after_callback;
};
std::vector<PendingFence> g_pending_fences;

// Requires g_mutex.
void WriteFence(u32 completion_va, u32 fence) {
  if (auto *word = GuestPtr<be_u32>(completion_va))
    *word = fence;
  ++g_fences_done;
}

// Render thread only: time spent running work since the last frame ended, and
// when the running item started; of that time, how much it waited (WaitScope).
Clock::duration g_busy{};
Clock::time_point g_item_start{};
Clock::duration g_waited{};
thread_local bool t_render_thread = false;
#if defined(__SWITCH__) && defined(__aarch64__)
// ssasr-nx (2026-11-27, the thread batch T1): the calling thread without a TLS call (__aarch64_read_tp is a call at
// every use on the Switch, batch 5 N14): its thread region's address, which the kernel keeps in tpidrro_el0, one per
// thread. The render thread's and the submit worker's, for SubmitRouted.
inline uintptr_t ThreadRegion() {
  uintptr_t tp;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
  return tp;
}
std::atomic<uintptr_t> g_render_region{0}, g_submit_region{0};
#endif

void Execute(Item &item) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kRunItem);  // blur-nx (2026-11-17)
  if (item.plain) {  // ssasr-nx (2026-11-26, batch 4 #1)
    if (item.locked) {
      std::lock_guard lock(State().mutex);
      item.plain(item.arg);
    } else {
      item.plain(item.arg);
    }
    return;
  }
  if (item.locked) {
    std::lock_guard lock(State().mutex);
    (*item.work)();
  } else {
    (*item.work)();
  }
}

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): Run with the ring. The items are
// taken in order as long as they are published, at most kRingRun before the done bookkeeping (Drain's count, the done
// waiters); with none ready the thread says it is going to sleep (g_waiting), looks once more, and sleeps in g_work_cv
// under g_mutex: a push that read g_waiting false after publishing (both sides store, then load, sequentially
// consistent: no store-load reordering) is seen by that last look, and one that read it true takes g_mutex before it
// notifies, so it cannot notify between the look and the wait.
constexpr size_t kRingRun = 256;
// banjo-nx (2026-11-20, render-thread batch, experiment #58): with the ring empty, the render thread looks again for up
// to this many microseconds (a YIELD between looks) before it sleeps: a push in that time is taken without the kernel's
// sleep and wake (svcWaitForAddress / svcSignalToAddress, tens of microseconds each way). 0: sleep at once, as before.
// It spends the render thread's core while it looks; measure it (tools/ab_halves.py) before keeping it.
void RunRing() {
  static const bool batch_clock = REXCVAR_GET(banjo_renut_batch_clock);
  static const bool prefetch = REXCVAR_GET(banjo_renut_prefetch_next);
  static const int spin_us = REXCVAR_GET(banjo_renut_ring_spin_us);
  uint64_t pos = g_dequeue.load(std::memory_order_relaxed);
  for (;;) {
    if (spin_us > 0 && !RingReady(pos)) {
      const Clock::time_point until = Clock::now() + std::chrono::microseconds(spin_us);
      while (!RingReady(pos) && Clock::now() < until) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
      }
    }
    if (!RingReady(pos)) {
      g_waiting.store(true, std::memory_order_seq_cst);
      bool stopping = false;
      if (AddressWakeOn()) {
        // blur-nx (2026-11-17, audit #11): the word read before the last look (see g_wake_word). With a wake batch,
        // draws queued without a wake are taken within 2 ms, as with g_work_cv; otherwise a 100 ms net.
        const int64_t slice_ns = WakeBatch() > 1 ? 2'000'000 : 100'000'000;
        for (;;) {
          const uint32_t word = g_wake_word.load(std::memory_order_seq_cst);
          if (RingReadySc(pos) || g_stop_flag.load(std::memory_order_acquire))
            break;
#if defined(__SWITCH__)
          RexSwitchWaitIfEqual(reinterpret_cast<volatile uint32_t *>(&g_wake_word), word, slice_ns);
#else
          (void)word;
          (void)slice_ns;
#endif
        }
        stopping = g_stop_flag.load(std::memory_order_acquire);
      } else if (!RingReadySc(pos)) {
        std::unique_lock lock(g_mutex);
        g_render_waiting = true;
        if (WakeBatch() > 1) {
          // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
          while (!RingReady(pos) && !g_stopping)
            g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
        } else {
          g_work_cv.wait(lock, [pos] { return RingReady(pos) || g_stopping; });
        }
        g_render_waiting = false;
        stopping = g_stopping;
      }
      g_waiting.store(false, std::memory_order_relaxed);
      if (!RingReady(pos)) {
        if (stopping)
          break;
        continue;
      }
    }
    const Clock::time_point start = Clock::now();
    if (batch_clock)
      g_item_start = start;
    size_t n = 0;
    Clock::time_point item_start = start;
    do {
      RingSlot &slot = g_ring[pos & (kRingSlots - 1)];
      if (prefetch && RingReady(pos + 1))
        PrefetchItem(g_ring[(pos + 1) & (kRingSlots - 1)].item);
      if (!batch_clock)
        g_item_start = item_start;
      Execute(slot.item);
      if (slot.item.plain)  // ssasr-nx (2026-11-26, batch 4 #1): nothing to destroy
        slot.item.plain = nullptr;
      else
        slot.item.work = nullptr;  // the closure's captures go now, as the batch's cleared vector freed them
      slot.seq.store(pos + kRingSlots, std::memory_order_release);
      ++pos;
      g_dequeue.store(pos, std::memory_order_relaxed);
      ++n;
      if (!batch_clock) {
        const Clock::time_point end = Clock::now();
        g_busy += end - item_start;
        item_start = end;
      }
    } while (n < kRingRun && RingReady(pos));
    if (batch_clock)
      g_busy += Clock::now() - start;
    bool wake;
    // blur-nx (2026-11-17, the Renut audit's #12, blur_renut_done_lockfree): the count added and the waiters read
    // without g_mutex (both sequentially consistent). A waiter counts itself under g_mutex before it tests its
    // condition: either this load sees it (and the wake takes g_mutex once, so the waiter is asleep in g_done_cv or
    // has not yet tested), or its test comes after this add and sees the new count.
    static const bool lockfree = REXCVAR_GET(blur_renut_done_lockfree);
    if (lockfree && !DoneNotifyAll()) {
      g_items_done.fetch_add(n, std::memory_order_seq_cst);
      wake = g_done_waiters.load(std::memory_order_seq_cst) > 0;
      if (wake) {
        std::lock_guard lock(g_mutex);
      }
    } else {
      std::lock_guard lock(g_mutex);
      g_items_done += n;
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_accepting.store(false, std::memory_order_release);
  g_done_cv.notify_all();
}

void Run() {
  {
    // banjo-nx: the thread may be a plain pthread on the Switch (Start), so it records its own id.
    std::lock_guard lock(g_mutex);
    g_thread_id = std::this_thread::get_id();
  }
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut render");
#endif
  banjo::renut_gpu::OnRenderThreadStart();  // banjo-nx: its core and priority on the Switch
  t_render_thread = true;
#if defined(__SWITCH__) && defined(__aarch64__)
  g_render_region.store(ThreadRegion(), std::memory_order_relaxed);  // ssasr-nx (the thread batch T1): SubmitRouted
#endif
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    RunRing();
    return;
  }
  std::vector<Item> batch;
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      g_render_waiting = true;
      if (WakeBatch() > 1) {
        // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
        while (g_queue.empty() && !g_stopping)
          g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_work_cv.wait(lock, [] { return !g_queue.empty() || g_stopping; });
      }
      g_render_waiting = false;
      if (g_queue.empty())
        break;
      batch.swap(g_queue);
    }
    // banjo-nx (2026-10-19, render-thread proposal 19): one clock read per item (the end of one is the start of the
    // next). Its other half, one State().mutex hold across consecutive locked items, hung the emulator run at the
    // first loading screen (a game thread's hook waits on the lock while the render thread's next item waits on it):
    // each item takes the lock on its own, as before.
    Clock::time_point start = Clock::now();
    static const bool batch_clock = REXCVAR_GET(banjo_renut_batch_clock);
    if (batch_clock) {
      // banjo-nx (2026-10-20, from sa2-nx): the batch's start for every item (QueueFrame's item counts its frame up to itself from
      // there and leaves the rest of the batch to the next frame), one clock read at the end.
      g_item_start = start;
      static const bool prefetch = REXCVAR_GET(banjo_renut_prefetch_next);
      const size_t n = batch.size();
      for (size_t i = 0; i < n; ++i) {
        if (prefetch && i + 1 < n)
          PrefetchItem(batch[i + 1]);
        Execute(batch[i]);
      }
      g_busy += Clock::now() - start;
    } else {
      for (Item &item : batch) {
        g_item_start = start;
        Execute(item);
        const Clock::time_point end = Clock::now();
        g_busy += end - start;
        start = end;
      }
    }
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += batch.size();
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
    batch.clear();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_done_cv.notify_all();
}

void StartSubmitWorker();  // ssasr-nx (2026-11-27, the thread batch T1): below

void Start() {
  std::call_once(g_start_once, [] {
    InstallLateInput();  // banjo-nx (2026-10-19, banjo_late_input): the delay function (it reads the key per frame)
    std::lock_guard lock(g_mutex);
    g_running = true;
    // dishonored-nx (port 105, dishonored_renut_lockfree_queue): latched before the thread is made and before
    // g_started's release; the ring, each slot free for its position.
    g_lockfree = REXCVAR_GET(dishonored_renut_lockfree_queue);
    g_notify_idle = banjo::renut_gpu::QueueNotifyIdleOn();
    g_wake_batch = WakeBatch();
    if (LockfreeOn()) {
      g_ring = new RingSlot[kRingSlots];
      for (uint64_t i = 0; i < kRingSlots; ++i)
        g_ring[i].seq.store(i, std::memory_order_relaxed);
      StartSubmitWorker();  // ssasr-nx (2026-11-27, the thread batch T1): before anything is accepted
      g_accepting.store(true, std::memory_order_release);
    }
    // Never joined or destroyed: the renderer's objects are left at exit too
    // (Video::BeginShutdown), and a joinable std::thread destroyed at exit
    // would terminate the process.
    // banjo-nx: a 1 MB stack on the Switch (the conversions and the driver run deep on this thread), never joined
    // (banjo_renut_tuning.h); Run records its id.
    banjo::renut_gpu::StartLongLivedThread(Run, 1024 * 1024);
    RGPU_INFO("Render thread started");
  });
  // dishonored-nx (port 105): release, read with acquire by Push: the ring's latch and the ring itself are seen with it
  // (a push into the ring takes no g_mutex, which made relaxed enough before).
  g_started.store(true, std::memory_order_release);
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): before a thread waits for the render thread, the draws queued without a
// wake are handed over. Requires g_mutex.
void WakeSubmitForWait();  // ssasr-nx (2026-11-27, the thread batch T1): below

void WakeForWaitLocked() {
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    WakeSubmitForWait();  // ssasr-nx (the thread batch T1): the worker's draws waiting for their batch, handed on now
    if (AddressWakeOn()) {  // blur-nx (2026-11-17, audit #11): the sleeper holds no g_mutex; g_waiting says it sleeps
      if (g_waiting.load(std::memory_order_seq_cst) && g_ring && RingReady(g_dequeue.load(std::memory_order_relaxed)))
        WakeRenderAddress();
      return;
    }
    if (g_render_waiting && g_ring && RingReady(g_dequeue.load(std::memory_order_relaxed)))
      g_work_cv.notify_one();
    return;
  }
  if (g_render_waiting && !g_queue.empty())
    g_work_cv.notify_one();
}

// vivapinata-nx (2026-11-11, vivapinata_tiles_once): SetBeforePushHook's function.
std::atomic<void (*)()> g_before_push{nullptr};
// banjo-nx (2026-11-23, banjo_renut_tile_twin_merge): a second one (queued.cpp's held tile twin), called first.
std::atomic<void (*)()> g_before_push2{nullptr};

// ssasr-nx (2026-11-27, batch 5 N14): the hooks read first and the thread only when one is set (t_render_thread is a
// call at every push on the Switch, __aarch64_read_tp, for two hooks that are null in a race). The same calls.
[[gnu::always_inline]] inline void BeforePush() {
  if (!(g_before_push2.load(std::memory_order_relaxed) || g_before_push.load(std::memory_order_relaxed))) [[likely]]
    return;
  if (t_render_thread)
    return;
  if (void (*hook)() = g_before_push2.load(std::memory_order_relaxed)) [[unlikely]]
    hook();
  if (void (*hook)() = g_before_push.load(std::memory_order_relaxed)) [[unlikely]]
    hook();
}

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): Push with the ring; false when the
// item was not queued (the ring is full and this is the render thread, which cannot wait for itself). The wake decision
// is Push's: a non-draw item, or WakeBatch draws not yet taken, wakes a render thread that is going to sleep (RunRing
// says why no wake is lost).
// ssasr-nx (2026-11-27, batch 5 H12): the slot filled by 'fill' (PushRing moves an Item in; a plain draw writes its
// four fields straight into the slot, no std::function moved or destroyed).
template <typename Fill>
bool PushRingWith(Fill &&fill, bool draw);

[[gnu::hot]] bool PushRing(Item &&item, bool draw) {
  return PushRingWith([&item](Item &slot_item) { slot_item = std::move(item); }, draw);
}

// ---------------------------------------------------------------------------
// ssasr-nx (2026-11-27, the thread batch T1 / T6 / T7; out/thread_plan.md): the submit worker. With
// ssasr_submit_worker, every push from a thread other than the render thread and the worker goes to the submit ring
// (the render ring's form: claimed positions, published by their sequence) and the worker takes the positions in order,
// runs an item's 'pre' (queued.cpp's finish of a staged draw capture) and pushes it on to the render ring: the render
// thread gets the same items in the same order, one stage later. The game thread's push is the same kind of push as
// before; what it no longer does is the capture's compare and delta (on the worker) and the render ring's publish and
// wakes (the worker's).
// ---------------------------------------------------------------------------
struct alignas(64) SubSlot {
  std::atomic<uint64_t> seq{0};
  Item item;
  void (*pre)(void *) = nullptr;
};
static_assert(sizeof(SubSlot) == 64, "a submit slot is one cache line");
constexpr uint64_t kSubSlots = uint64_t(1) << 12;  // 256 KB; the worker is a few items behind at most
SubSlot *g_sub_ring = nullptr;                    // made by Start before the worker runs
EnqueueLine g_sub_enqueue_line;
std::atomic<uint64_t> &g_sub_enqueue = g_sub_enqueue_line.pos;
alignas(64) std::atomic<uint64_t> g_sub_dequeue{0};    // the next position the worker takes (its own)
alignas(64) std::atomic<uint64_t> g_sub_forwarded{0};  // the positions handed on to the render ring (Drain)
WaitingLine g_sub_waiting_line;
std::atomic<bool> &g_sub_waiting = g_sub_waiting_line.flag;
alignas(64) std::atomic<uint32_t> g_sub_wake_word{0};
std::atomic<bool> g_sub_stop{false};
std::mutex g_sub_mutex;  // the condition variable's (off the Switch)
std::condition_variable g_sub_cv;
std::atomic<uint64_t> g_sub_pre_count{0}, g_sub_sleeps{0}, g_sub_wakes{0}, g_sub_full{0};
thread_local bool t_submit_worker = false;

inline bool SubReady(uint64_t pos) {
  return g_sub_ring[pos & (kSubSlots - 1)].seq.load(std::memory_order_acquire) == pos + 1;
}
inline bool SubReadySc(uint64_t pos) {
  return g_sub_ring[pos & (kSubSlots - 1)].seq.load(std::memory_order_seq_cst) == pos + 1;
}
// The submit ring takes this thread's pushes: it is on, and this is neither the render thread nor the worker.
[[gnu::always_inline]] inline bool SubmitRouted() {
  if (!g_push_config.submit) [[likely]]
    return false;
#if defined(__SWITCH__) && defined(__aarch64__)
  const uintptr_t me = ThreadRegion();
  return me != g_render_region.load(std::memory_order_relaxed) && me != g_submit_region.load(std::memory_order_relaxed);
#else
  return !t_render_thread && !t_submit_worker;
#endif
}
void WakeSubmit() {
  g_sub_wakes.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
  if (AddressWakeOn()) {
    g_sub_wake_word.fetch_add(1, std::memory_order_seq_cst);
    RexSwitchWakeAddress(reinterpret_cast<volatile uint32_t *>(&g_sub_wake_word), 1);
    return;
  }
#endif
  {
    std::lock_guard lock(g_sub_mutex);
  }
  g_sub_cv.notify_one();
}

// A submit slot claimed (its position in 'pos'): the order of the claims is the order of the work.
[[gnu::hot]] SubSlot *SubmitClaim(uint64_t &pos) {
  pos = g_sub_enqueue.load(std::memory_order_relaxed);
  SubSlot *slot;
  for (;;) {
    slot = &g_sub_ring[pos & (kSubSlots - 1)];
    const uint64_t seq = slot->seq.load(std::memory_order_acquire);
    const int64_t diff = int64_t(seq - pos);
    if (diff == 0) {
      if (g_sub_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        break;
    } else if (diff < 0) {
      g_sub_full.fetch_add(1, std::memory_order_relaxed);
      if (g_sub_waiting.load(std::memory_order_seq_cst))
        WakeSubmit();
      std::this_thread::yield();
      pos = g_sub_enqueue.load(std::memory_order_relaxed);
    } else {
      pos = g_sub_enqueue.load(std::memory_order_relaxed);
    }
  }
  __builtin_prefetch(&g_sub_ring[(pos + kRingAhead) & (kSubSlots - 1)], 1);
  return slot;
}
void SubmitPublish(SubSlot *slot, uint64_t pos);

// ssasr-nx (2026-11-27, the thread batch F3, ssasr_draw_runs): the drawing thread's staged draws in runs of up to
// kRunDraws, one submit slot each. A run's slot is claimed at its first draw, so its place in the order is the place
// of that draw's push, as before; it is published when the run is full, before the thread's next push of anything
// else, before it waits for the render thread (Drain, a frame, a fence) and before it blocks or yields in the kernel
// (rex::kernel's guest block hook): it never blocks while its open run holds up the worker and what others queued
// after it. One claim, one line and one publish for up to six draws instead of one each (the push's cost was its
// line traffic: the load after its stlr, 30 % of its samples, and its slot's sequence, 10 %). The worker hands each
// draw on as its own item, as the draw pushes did: the same items in the same order.
constexpr u32 kRunDraws = 6;
struct alignas(64) DrawRun {
  void (*fn)(void *) = nullptr;
  void (*pre)(void *) = nullptr;
  u32 count = 0;
  u32 pad = 0;
  struct {
    void *arg;
    const void *prefetch1;
  } draw[kRunDraws] = {};
};
static_assert(sizeof(DrawRun) == 128, "a draw run is two lines");
constexpr uint64_t kRuns = kSubSlots;  // (as many as the slots: a run's buffer is free again before its slot is)
DrawRun *g_runs = nullptr;
std::atomic<uintptr_t> g_run_owner{0};  // the one thread that makes runs (the first to push a staged draw)
thread_local bool t_run_owner = false;  // (off the Switch: the same, by TLS)
// The owner's own state.
uint64_t g_run_next = 0;          // the run being filled (index into g_runs)
SubSlot *g_run_slot = nullptr;    // its claimed submit slot; null: none open
uint64_t g_run_pos = 0;           // that slot's position
alignas(64) std::atomic<uint64_t> g_runs_done{0};  // runs the worker has handed on (their buffers free)
std::atomic<uint64_t> g_runs_published{0}, g_run_draws{0};
void RunMarker(void *) {}  // a run's slot's 'pre' (never called: the worker's sign)

[[gnu::always_inline]] inline bool RunOwnerIsMe() {
#if defined(__SWITCH__) && defined(__aarch64__)
  const uintptr_t owner = g_run_owner.load(std::memory_order_relaxed);
  return owner && owner == ThreadRegion();
#else
  return t_run_owner;
#endif
}

// The open run published (the owner's).
void FlushRun() {
  SubSlot *const slot = g_run_slot;
  if (!slot)
    return;
  g_run_slot = nullptr;
  DrawRun &run = g_runs[g_run_next % kRuns];
  ++g_run_next;
  slot->item.locked = true;
  slot->item.plain = nullptr;
  slot->item.arg = &run;
  slot->item.prefetch[0] = run.draw[0].arg;
  slot->item.prefetch[1] = run.draw[0].prefetch1;
  slot->item.submit_draw = true;
  slot->pre = &RunMarker;
  g_runs_published.fetch_add(1, std::memory_order_relaxed);
  g_run_draws.fetch_add(run.count, std::memory_order_relaxed);
  SubmitPublish(slot, g_run_pos);
}
[[gnu::always_inline]] inline void FlushRunIfOwner() {
  if (g_runs && g_run_slot && RunOwnerIsMe())
    FlushRun();
}
// The guest block hook (rex::kernel): a run held by this thread published before it blocks.
void FlushRunBeforeBlock() {
  FlushRunIfOwner();
}

// A push into the submit ring: claimed, filled, published; the worker woken when it sleeps and this is not a draw, or
// a wake batch of draws is waiting, or someone waits for the render thread (a frame, a fence, Drain).
template <typename Fill>
[[gnu::hot]] bool SubmitPushWith(Fill &&fill, bool draw, void (*pre)(void *)) {
  FlushRunIfOwner();  // ssasr-nx (the thread batch F3): this thread's open run first (its slot is ahead of this one)
  uint64_t pos;
  SubSlot *slot = SubmitClaim(pos);
  fill(slot->item);
  slot->item.submit_draw = draw;
  slot->pre = pre;
  SubmitPublish(slot, pos);
  return true;
}

// The publish and the worker's wake.
[[gnu::hot]] void SubmitPublish(SubSlot *slot, uint64_t pos) {
  slot->seq.store(pos + 1, std::memory_order_seq_cst);
  // The render ring's N4: a push below the position a look said cannot need a wake skips the ordered load. Every item
  // waits for the wake batch here, draws or not (the PC check's first run: 101,049 wakes for 249,673 items, the
  // non-draw items each waking a sleeping worker, a kernel call on the Switch): a thread that waits for the render
  // thread wakes the worker itself (WakeForWaitLocked), and the worker's 2 ms slice bounds the rest. Same order.
  std::atomic<uint64_t> &next = g_sub_enqueue_line.next_wake_check;
  bool wake;
  if (g_done_waiters.load(std::memory_order_relaxed) == 0) {
    const uint64_t next_check = next.load(std::memory_order_relaxed);
    if (pos < next_check && next_check - pos <= g_wake_batch) {
      wake = false;
    } else if (!g_sub_waiting.load(std::memory_order_seq_cst)) {
      wake = false;
      next.store(pos + g_wake_batch, std::memory_order_relaxed);
    } else {
      const uint64_t done = g_sub_dequeue.load(std::memory_order_relaxed);
      wake = pos + 1 - done >= g_wake_batch;
      next.store(wake ? pos + 1 : done + g_wake_batch - 1, std::memory_order_relaxed);
    }
  } else {
    wake = g_sub_waiting.load(std::memory_order_seq_cst);
  }
  if (wake)
    WakeSubmit();
}

template <typename Fill>
[[gnu::hot]] bool PushRingWith(Fill &&fill, bool draw) {
  // ssasr-nx (2026-11-27, the thread batch T1): the submit ring first, while it is on (the worker pushes here itself).
  if (SubmitRouted())
    return SubmitPushWith(fill, draw, nullptr);
  uint64_t pos = g_enqueue.load(std::memory_order_relaxed);
  RingSlot *slot;
  for (;;) {
    slot = &g_ring[pos & (kRingSlots - 1)];
    const uint64_t seq = slot->seq.load(std::memory_order_acquire);
    const int64_t diff = int64_t(seq - pos);
    if (diff == 0) {
      if (g_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        break;
    } else if (diff < 0) {
      // Full: the render thread is kRingSlots items behind.
      if (t_render_thread) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
          RGPU_WARN("Render thread queue full ({} items): the render thread's own item ran at once "
                    "(dishonored_renut_lockfree_queue)", kRingSlots);
        return false;
      }
      if (g_waiting.load(std::memory_order_seq_cst)) {  // (a signal is a kernel call on the Switch: only to a sleeper)
        if (AddressWakeOn()) {
          WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
        } else {
          {
            std::lock_guard lock(g_mutex);
          }
          g_work_cv.notify_one();
        }
      }
      std::this_thread::yield();
      pos = g_enqueue.load(std::memory_order_relaxed);
    } else {
      pos = g_enqueue.load(std::memory_order_relaxed);
    }
  }
  // The slot kRingAhead positions on, into this core's cache for writing while this one is filled (its line was last
  // the render thread's).
  __builtin_prefetch(&g_ring[(pos + kRingAhead) & (kRingSlots - 1)], 1);
  // ssasr-nx (2026-11-27, batch 5 N5): and its second line (a slot is 128 bytes: a plain draw's last two words, at 64
  // and 72, are there). The console's batch4d profile: 42 of PushRing's 75 samples on the ldar of g_waiting after the
  // publish, which waits for the fill's stores, the second line's a miss on the line the render thread wrote last.
  if (sizeof(RingSlot) > 64 && ::ssasr::host50::On(::ssasr::host50::N(5)))  // (L123: a slot is one line now)
    __builtin_prefetch(reinterpret_cast<const char *>(&g_ring[(pos + kRingAhead) & (kRingSlots - 1)]) + 64, 1);
  fill(slot->item);
  // Published, then g_waiting read: a sequentially consistent store and load (stlr, ldar: no dmb), RunRing's order.
  // ssasr-nx (2026-11-27, batch 5 N20): on AArch64 the publish as a store barrier and a plain store (dmb ishst; str):
  // the console's A57 holds every load after an stlr until the stores before it have drained (5E: 37 samples on the
  // first load after the publish, a word on a line of its own). The render thread's acquire of the sequence still sees
  // the item: every store before the barrier is observed before the sequence's. The one order the release also gave,
  // the publish before the load of g_waiting (RunRing's guarantee against a lost wake), is a full barrier before that
  // load below, where one is made (with N4, about one draw in the wake batch).
  const bool store_barrier = kStoreBarrierPublish && ::ssasr::host50::On(::ssasr::host50::N(20));
  if (store_barrier) {
#if defined(__aarch64__)
    __asm__ volatile("dmb ishst" ::: "memory");
#endif
    slot->seq.store(pos + 1, std::memory_order_relaxed);
  } else {
    slot->seq.store(pos + 1, std::memory_order_seq_cst);
  }
  // (N20: the store before any load of g_waiting, as the release gave it)
  const auto waiting_after_publish = [store_barrier] {
    if (store_barrier)
      std::atomic_thread_fence(std::memory_order_seq_cst);
    return g_waiting.load(std::memory_order_seq_cst);
  };
  // blur-nx (2026-11-18, the Renut host batch's #28, blur_renut_wake_batch_all / A/B 238): with a wake batch (the render
  // thread then looks every 2 ms by itself), any item may wait for the batch as draws do: command buffer runs and
  // resolves woke it at once, each a kernel call on the pushing thread (the Game Master's), with the render thread idle
  // half the time.
  const bool as_draw = draw || (g_wake_batch > 1 && ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kWakeBatchAll));
  // ssasr-nx (2026-11-27, batch 5 N4): the ordered load of g_waiting (an ldar after the publish's stlr, which waits for
  // every store before it: 42 of PushRing's 75 samples in the console's batch4d profile) left out at the draws that
  // cannot wake. A look that read g_waiting false after publishing position P: the render thread cannot sleep before it
  // has taken P (its last look before sleeping comes after its store of true, so it sees P), so a draw at a position
  // below P + batch finds at most batch - 1 queued after its sleep and wakes nothing. A look that read it true with
  // 'done' taken and no wake: no draw below done + batch - 1 has a batch queued (done only grows). Any other draw
  // looks as before; g_dequeue is read no more often than before (only after g_waiting read true). The same wakes with
  // one pusher (the race's game thread); with several, a slot below P still being filled by another can let the render
  // thread sleep before P, and a wake may then come that other push's few positions later (its 2 ms slice still
  // bounds the wait). The picture is the same either way.
  bool wake;
  if (as_draw && g_notify_idle && ::ssasr::host50::On(::ssasr::host50::N(4))) {
    std::atomic<uint64_t> &next = g_enqueue_line.next_wake_check;
    const uint64_t next_check = next.load(std::memory_order_relaxed);
    if (pos < next_check && next_check - pos <= g_wake_batch) {  // (a valid one is never further)
      wake = false;
    } else if (!waiting_after_publish()) {
      wake = false;
      next.store(pos + g_wake_batch, std::memory_order_relaxed);
    } else {
      const uint64_t done = g_dequeue.load(std::memory_order_relaxed);
      wake = pos + 1 - done >= g_wake_batch;
      next.store(wake ? pos + 1 : done + g_wake_batch - 1, std::memory_order_relaxed);
    }
  } else {
    wake = (waiting_after_publish() &&
            (!as_draw || pos + 1 - g_dequeue.load(std::memory_order_relaxed) >= g_wake_batch)) ||
           !g_notify_idle;
  }
  if (wake) {
    const bool hb_census = ::renut::gpu::host_batch::CensusOn();  // blur-nx (2026-11-18, the census's #28)
    const uint64_t t0 = hb_census ? ::renut::gpu::host_batch::Ticks() : 0;
    if (AddressWakeOn()) {
      WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
    } else {
      {
        std::lock_guard lock(g_mutex);
      }
      g_work_cv.notify_one();
    }
    if (hb_census) {
      ::renut::gpu::host_batch::Add(draw ? ::renut::gpu::host_batch::kWakesDraw : ::renut::gpu::host_batch::kWakesOther);
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kWakeTicks, ::renut::gpu::host_batch::Ticks() - t0);
    }
  }
  return true;
}

// ssasr-nx (2026-11-26, batch 4 #1): the ring's push of a ready item (Push's and PushPlain's).
void PushItem(Item &&item, bool draw) {
  if (g_accepting.load(std::memory_order_acquire) && PushRing(std::move(item), draw))
    return;
  Execute(item);  // stopping, or the render thread's own item in a full ring: run here, as Push does
}

void Push(std::function<void()> work, bool locked, const void *prefetch0 = nullptr,
          const void *prefetch1 = nullptr, bool draw = false) {
  BeforePush();  // vivapinata-nx (2026-11-11): a held tile run goes in first
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]]
    Start();  // bangaio-nx (2026-10-29, v6): only until it has run (g_started; acquire since port 105's ring)
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    Item item{MakeWork(std::move(work)), locked, false, {prefetch0, prefetch1}};
    if (g_accepting.load(std::memory_order_acquire) && PushRing(std::move(item), draw))
      return;
    // Stopping, or the render thread's own item in a full ring: run here, as a push while stopping did.
    Execute(item);
    return;
  }
  {
    std::unique_lock lock(g_mutex);
    // (Work queued by the render thread's own work runs after it.)
    if (g_running && !g_stopping) {
      g_queue.push_back({MakeWork(std::move(work)), locked, false, {prefetch0, prefetch1}});
      ++g_items_queued;
      // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): a draw wakes a sleeping render thread once enough are queued.
      const bool wake = (g_render_waiting && (!draw || g_queue.size() >= WakeBatch())) ||
                        !banjo::renut_gpu::QueueNotifyIdleOn();
      lock.unlock();
      if (wake)
        g_work_cv.notify_one();
      return;
    }
  }
  // Shutting down: nothing takes work any more, so run it here.
  Item item{MakeWork(std::move(work)), locked, false, {}};
  Execute(item);
}

// ssasr-nx (2026-11-27, the thread batch T1): the submit worker's loop. The positions in order: an item's 'pre' (a
// staged capture finished: queued.cpp), then the render ring's push (its publish and its wakes, T6). While anyone
// waits for the render thread (a frame's, a fence's, Drain), every item it hands on wakes the render thread (a draw no
// longer waits for its wake batch: the waiter's WakeForWaitLocked looked at the render ring before the item was in it).
// With the ring empty it says it is going to sleep, looks once more, and sleeps (the render thread's protocol, RunRing).
void SubmitRun() {
#if defined(__SWITCH__) && defined(__aarch64__)
  g_submit_region.store(ThreadRegion(), std::memory_order_relaxed);
#endif
  t_submit_worker = true;
  banjo::renut_gpu::OnSubmitWorkerStart(REXCVAR_GET(ssasr_submit_worker_core));
  uint64_t pos = g_sub_dequeue.load(std::memory_order_relaxed);
  for (;;) {
    if (!SubReady(pos)) {
      g_sub_waiting.store(true, std::memory_order_seq_cst);
      g_sub_sleeps.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
      if (AddressWakeOn()) {
        for (;;) {
          const uint32_t word = g_sub_wake_word.load(std::memory_order_seq_cst);
          if (SubReadySc(pos) || g_sub_stop.load(std::memory_order_acquire))
            break;
          RexSwitchWaitIfEqual(reinterpret_cast<volatile uint32_t *>(&g_sub_wake_word), word, 2'000'000);
        }
      } else
#endif
      {
        std::unique_lock lock(g_sub_mutex);
        while (!SubReadySc(pos) && !g_sub_stop.load(std::memory_order_acquire))
          g_sub_cv.wait_for(lock, std::chrono::milliseconds(2));
      }
      g_sub_waiting.store(false, std::memory_order_relaxed);
      if (!SubReady(pos)) {
        if (g_sub_stop.load(std::memory_order_acquire))
          break;
        continue;
      }
    }
    do {
      SubSlot &slot = g_sub_ring[pos & (kSubSlots - 1)];
      Item item = std::move(slot.item);
      void (*const pre)(void *) = slot.pre;
      slot.item.plain = nullptr;
      slot.item.arg = nullptr;
      slot.pre = nullptr;
      slot.seq.store(pos + kSubSlots, std::memory_order_release);
      ++pos;
      g_sub_dequeue.store(pos, std::memory_order_relaxed);
      if (pre == &RunMarker) {  // ssasr-nx (the thread batch F3): a draw run, each draw handed on as its own item
        DrawRun *const run = static_cast<DrawRun *>(item.arg);
        for (u32 k = 0; k < run->count; ++k) {
          void *const arg = run->draw[k].arg;
          if (run->pre) {
            run->pre(arg);
            g_sub_pre_count.fetch_add(1, std::memory_order_relaxed);
          }
          Item each;
          each.locked = true;
          each.prefetch[0] = arg;
          each.prefetch[1] = run->draw[k].prefetch1;
          each.plain = run->fn;
          each.arg = arg;
          const bool draw_each = g_done_waiters.load(std::memory_order_seq_cst) == 0;
          if (!(g_accepting.load(std::memory_order_acquire) && PushRing(std::move(each), draw_each)))
            Execute(each);
        }
        run->count = 0;
        g_runs_done.fetch_add(1, std::memory_order_release);
        g_sub_forwarded.store(pos, std::memory_order_release);
        continue;
      }
      if (pre) {
        pre(item.arg);
        g_sub_pre_count.fetch_add(1, std::memory_order_relaxed);
      }
      const bool draw = item.submit_draw && g_done_waiters.load(std::memory_order_seq_cst) == 0;
      item.submit_draw = false;
      if (!(g_accepting.load(std::memory_order_acquire) && PushRing(std::move(item), draw)))
        Execute(item);  // stopping: run here, as a push while stopping does
      g_sub_forwarded.store(pos, std::memory_order_release);
    } while (SubReady(pos));
    // A waiter that counted itself after this batch's last look at g_done_waiters (it woke this thread before counting:
    // QueueFrame, Drain, the fence waits): the render thread, asleep with items, is woken now rather than at its slice.
    if (g_done_waiters.load(std::memory_order_seq_cst) > 0 && g_waiting.load(std::memory_order_seq_cst) &&
        RingReady(g_dequeue.load(std::memory_order_relaxed))) {
      if (AddressWakeOn()) {
        WakeRenderAddress();
      } else {
        {
          std::lock_guard lock(g_mutex);
        }
        g_work_cv.notify_one();
      }
    }
  }
}

// Start's part (under g_mutex, before the render ring accepts): the ring, each slot free for its position, and the
// worker; then pushes are routed to it.
void StartSubmitWorker() {
  if (!LockfreeOn() || !REXCVAR_GET(ssasr_submit_worker))
    return;
  g_sub_ring = new SubSlot[kSubSlots];
  for (uint64_t i = 0; i < kSubSlots; ++i)
    g_sub_ring[i].seq.store(i, std::memory_order_relaxed);
  if (REXCVAR_GET(ssasr_draw_runs)) {  // ssasr-nx (the thread batch F3)
    g_runs = new DrawRun[kRuns];
    rex::kernel::SetGuestBlockHook(&FlushRunBeforeBlock);
  }
  banjo::renut_gpu::StartLongLivedThread(SubmitRun, 256 * 1024);
  g_push_config.submit = true;
  RGPU_INFO("Submit worker started (ssasr_submit_worker): the game's pushes go through it to the render thread{}",
            g_runs ? "; staged draws in runs of up to 6 (ssasr_draw_runs)" : "");
}

// A thread about to wait for the render thread (WakeForWaitLocked): a sleeping worker with items is woken (it then
// hands them on with a wake each, g_done_waiters being counted by then or soon after: the 2 ms slices bound the rest).
void WakeSubmitForWait() {
  if (g_push_config.submit && g_sub_waiting.load(std::memory_order_seq_cst) &&
      SubReady(g_sub_dequeue.load(std::memory_order_relaxed)))
    WakeSubmit();
}

// Waits until the worker has handed on every push claimed so far (Drain's first half).
void DrainSubmit() {
  if (!g_push_config.submit || !SubmitRouted())
    return;
  const uint64_t target = g_sub_enqueue.load(std::memory_order_acquire);
  while (g_sub_forwarded.load(std::memory_order_acquire) < target) {
    if (g_sub_waiting.load(std::memory_order_seq_cst))
      WakeSubmit();
    std::this_thread::yield();
  }
}

} // namespace

// ssasr-nx (2026-11-27, the thread batch T1)
bool SubmitWorkerOn() {
  static const bool on = REXCVAR_GET(ssasr_submit_worker) && REXCVAR_GET(dishonored_renut_lockfree_queue);
  return on;
}

bool SetterLogOn() {  // ssasr-nx (2026-11-27, the thread batch F1)
  static const bool on = SubmitWorkerOn() && REXCVAR_GET(ssasr_setter_log);
  return on;
}

void QueuePrefetchedPlainPre(const void *prefetch0, const void *prefetch1, void (*fn)(void *), void *arg,
                             void (*pre)(void *)) {
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]]
    Start();
  // ssasr-nx (2026-11-27, the thread batch F3): into the drawing thread's run (prefetch0 is the capture: 'arg').
  if (g_runs && pre && prefetch0 == arg && SubmitRouted() && g_accepting.load(std::memory_order_acquire)) {
    if (!RunOwnerIsMe()) {
      uintptr_t none = 0;
#if defined(__SWITCH__) && defined(__aarch64__)
      g_run_owner.compare_exchange_strong(none, ThreadRegion(), std::memory_order_relaxed);
#else
      if (g_run_owner.compare_exchange_strong(none, 1, std::memory_order_relaxed))
        t_run_owner = true;
#endif
    }
    if (RunOwnerIsMe()) {
      BeforePush();  // (the hooks' held work is a push of its own: it publishes this thread's open run before it)
      if (g_run_slot) {
        const DrawRun &open = g_runs[g_run_next % kRuns];
        if (open.fn != fn || open.pre != pre)
          FlushRun();
      }
      if (!g_run_slot) {
        while (g_run_next - g_runs_done.load(std::memory_order_acquire) >= kRuns) {
          if (g_sub_waiting.load(std::memory_order_seq_cst))
            WakeSubmit();
          std::this_thread::yield();
        }
        DrawRun &fresh = g_runs[g_run_next % kRuns];
        fresh.fn = fn;
        fresh.pre = pre;
        fresh.count = 0;
        g_run_slot = SubmitClaim(g_run_pos);
      }
      DrawRun &run = g_runs[g_run_next % kRuns];
      run.draw[run.count].arg = arg;
      run.draw[run.count].prefetch1 = prefetch1;
      if (++run.count == kRunDraws)
        FlushRun();
      return;
    }
  }
  if (!pre || !SubmitRouted() || !g_accepting.load(std::memory_order_acquire)) {
    if (pre)
      pre(arg);  // (the worker is off, or this thread is not routed: finished here, then queued as before)
    QueuePrefetchedPlain(prefetch0, prefetch1, fn, arg);
    return;
  }
  BeforePush();
  const auto fill = [&](Item &slot_item) {
    slot_item.locked = true;
    slot_item.prefetch[0] = prefetch0;
    slot_item.prefetch[1] = prefetch1;
    slot_item.plain = fn;
    slot_item.arg = arg;
  };
  SubmitPushWith(fill, true, pre);
}

std::string SubmitWorkerReport() {
  if (!g_push_config.submit)
    return std::string();
  return fmt::format("; submit worker: {} items handed on, {} captures finished, {} sleeps, {} wakes, {} full waits{}",
                     g_sub_forwarded.load(std::memory_order_relaxed), g_sub_pre_count.load(std::memory_order_relaxed),
                     g_sub_sleeps.load(std::memory_order_relaxed), g_sub_wakes.load(std::memory_order_relaxed),
                     g_sub_full.load(std::memory_order_relaxed),
                     g_runs ? fmt::format("; draw runs: {} published, {} draws in them",
                                          g_runs_published.load(std::memory_order_relaxed),
                                          g_run_draws.load(std::memory_order_relaxed))
                            : std::string());
}

void SetBeforePushHook(void (*hook)()) {
  g_before_push.store(hook, std::memory_order_relaxed);
}

void SetBeforePushHook2(void (*hook)()) {
  g_before_push2.store(hook, std::memory_order_relaxed);
}

void RunBeforePushHook(int which) {
  if (void (*hook)() = (which == 2 ? g_before_push2 : g_before_push).load(std::memory_order_relaxed))
    hook();
}

void Queue(std::function<void()> work) {
  Push(std::move(work), true);
}

void QueueUnlocked(std::function<void()> work) {
  Push(std::move(work), false);
}

void QueuePrefetched(const void *prefetch0, const void *prefetch1, std::function<void()> work) {
  Push(std::move(work), true, prefetch0, prefetch1, true);  // banjo-nx (from sa2-nx): the game's draws (banjo_renut_wake_batch)
}

// ssasr-nx (2026-11-26, batch 4 #1, ssasr_renut_host50): a draw as a plain item (a function and its argument).
[[gnu::hot]] void QueuePrefetchedPlain(const void *prefetch0, const void *prefetch1, void (*fn)(void *), void *arg) {
  namespace h50 = ::ssasr::host50;
  if (h50::CensusOn()) {
    h50::Add(h50::kPushes);
    if (h50::On(1) && LockfreeOn())
      h50::Add(h50::kPlainPushes);
  }
  if (!h50::On(1) || !LockfreeOn()) {
    Push([fn, arg] { fn(arg); }, true, prefetch0, prefetch1, true);
    return;
  }
  BeforePush();
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]]
    Start();
  if (h50::On(h50::H(12)) && g_accepting.load(std::memory_order_acquire)) {
    // ssasr-nx (2026-11-27, batch 5 H12): straight into the slot (its std::function is empty: the render thread
    // empties it after running a std::function item, and a plain one never fills it).
    const auto fill = [&](Item &slot_item) {
      slot_item.locked = true;
      slot_item.prefetch[0] = prefetch0;
      slot_item.prefetch[1] = prefetch1;
      slot_item.plain = fn;
      slot_item.arg = arg;
    };
    if (PushRingWith(fill, true))
      return;
  }
  Item item;
  item.locked = true;
  item.prefetch[0] = prefetch0;
  item.prefetch[1] = prefetch1;
  item.plain = fn;
  item.arg = arg;
  PushItem(std::move(item), true);
}

void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback) {
  QueueUnlocked([completion_va, fence, after_callback] {
    ::renut::gpu::SettleExportsForFence();  // port 170 (banjo_fence_waits_exports; nothing when off)
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      // Fences complete in order: behind one still waiting, this one waits too.
      if (g_pending_fences.empty() && after_callback <= g_callbacks_run)
        WriteFence(completion_va, fence);
      else
        g_pending_fences.push_back({completion_va, fence, after_callback});
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

void QueueCallbackMarker(uint64_t seq) {
  QueueUnlocked([seq] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_callbacks_reached = std::max(g_callbacks_reached, seq);
      ++g_fences_done;  // wakes WaitForFenceProgress: there is a callback to run
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

uint64_t CallbacksReached() {
  std::lock_guard lock(g_mutex);
  return g_callbacks_reached;
}

void CallbacksRun(uint64_t seq) {
  bool wake;
  {
    std::lock_guard lock(g_mutex);
    g_callbacks_run = std::max(g_callbacks_run, seq);
    size_t written = 0;
    for (; written < g_pending_fences.size(); ++written) {
      const PendingFence &p = g_pending_fences[written];
      if (p.after_callback > g_callbacks_run)
        break;
      WriteFence(p.completion_va, p.fence);
    }
    g_pending_fences.erase(g_pending_fences.begin(), g_pending_fences.begin() + written);
    wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
  }
  if (wake)
    g_done_cv.notify_all();
}

bool WaitForFenceProgress(std::chrono::milliseconds timeout) {
  BeforePush();  // vivapinata-nx (2026-11-11): nothing waits on work still held back
  FlushRunIfOwner();  // ssasr-nx (the thread batch F3)
  std::unique_lock lock(g_mutex);
  if (!g_running || g_stopping)
    return false;
  const uint64_t seen = g_fences_done;
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait_for(lock, timeout,
                     [seen] { return g_fences_done != seen || !g_running || g_stopping; });
  --g_done_waiters;
  return g_running && !g_stopping;
}

// banjo-nx (2026-10-19, input latency, banjo_late_input): the game reads its controller at the start of a frame, and
// when the render thread is the slower of the two its frame then waits: the Swap below waits for the frame before to be
// done. The slack of a frame is that wait plus the delay its read already had: how much later the read could have been
// without the frame reaching the render thread any later. The next frame's first read is delayed by the least slack of
// the last kSlackFrames frames less kLateInputMarginUs (rex::latency::SetFrameReadDelayFunction): a frame whose game
// work runs long eats into the margin, and a run of them shrinks the delay; with the game thread or the frame cap the
// limit there is no wait, and the delay goes to 0.
namespace {
constexpr uint32_t kSlackFrames = 16;
constexpr uint64_t kLateInputMarginUs = 2000;
// banjo-nx (2026-10-19, latency batch 3): the most the read is delayed: 80 % of the game's frame interval (averaged over
// the frames at QueueFrame), at most kLateInputCeilingUs. A fixed 30 ms held it back where the GPU is the limit: the
// console's 18 fps scenes (package 35) delayed 30.0 ms with more slack than that.
constexpr uint64_t kLateInputCeilingUs = 60000;
std::atomic<uint64_t> g_frame_interval_us{0};
uint64_t g_last_queue_us = 0;
uint64_t g_slack_us[kSlackFrames] = {};
uint32_t g_slack_next = 0;
uint32_t g_slack_count = 0;
std::atomic<uint32_t> g_applied_delay_us{0};
std::atomic<uint64_t> g_delay_total_us{0}, g_delay_frames{0};

// banjo-nx (2026-10-20, package 36s): banjo_late_input_gpu (36l-36r), a second slack from the GPU queue's wait, is gone:
// on the console it gained nothing at the 30 fps cap and, in scenes just able to hold 30, its delay ratcheted up to
// 29 ms and cost about 6 fps (36q). docs/deko3d-renderer.md section 25.

uint32_t LateInputDelay() {
  if (!banjo::renut_gpu::LateInputOn() || g_slack_count < kSlackFrames) {
    g_applied_delay_us.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t least = UINT64_MAX;
  for (uint64_t s : g_slack_us)
    least = std::min(least, s);
  const uint64_t interval = g_frame_interval_us.load(std::memory_order_relaxed);
  const uint64_t ceiling = std::min(kLateInputCeilingUs, interval * 4 / 5);
  // sa2-nx (2026-10-28, sa2_renut_queue_drain, after ketsui-nx's display guard): with double buffering, 1 ms more margin
  // at each image late at its refresh (0.25 ms back after 180 in time), so a delayed read cannot push the frames back
  // into the late queue; 0 otherwise.
  const uint64_t margin = kLateInputMarginUs + banjo::display_sync::LateInputGuardUs();
  const uint64_t delay = least > margin ? std::min(least - margin, ceiling) : 0;
  g_applied_delay_us.store(uint32_t(delay), std::memory_order_relaxed);
  g_delay_total_us.fetch_add(delay, std::memory_order_relaxed);
  g_delay_frames.fetch_add(1, std::memory_order_relaxed);
  return uint32_t(delay);
}
}  // namespace

void InstallLateInput() {
  rex::latency::SetFrameReadDelayFunction(&LateInputDelay);
}

std::string LateInputReport() {
  const uint64_t frames = g_delay_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_total_us.exchange(0, std::memory_order_relaxed);
  return frames ? fmt::format("late input: the first controller read of a frame delayed {:.1f} ms on average ({} "
                              "frames)",
                              double(total) / 1000.0 / double(frames), frames)
                : std::string("late input: no delay");
}

uint64_t QueueFrame(std::function<void()> work) {
  {
    std::lock_guard lock(g_mutex);
    ++g_frames_queued;
  }
  QueueUnlocked([work = std::move(work)] {
    if (static const int32_t delay = REXCVAR_GET(rr6_diag_render_delay_ms); delay > 0)  // rr6-nx
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    work();
    // The frame's work includes this item's (the present); Run adds the whole
    // item afterwards, so what is left of it counts to the next frame.
    const Clock::duration so_far = Clock::now() - g_item_start;
    const double busy_ms =
        std::chrono::duration<double, std::milli>(g_busy + so_far - g_waited).count();
    g_busy = -so_far;
    g_waited = {};
    frame_stats::OnRenderThreadFrame(busy_ms);
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      ++g_frames_done;
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
  const auto waiting = std::chrono::steady_clock::now();
  std::unique_lock lock(g_mutex);
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [] {
    static const uint64_t ahead = std::min<uint64_t>(uint64_t(REXCVAR_GET(rr6_frames_ahead)), kMaxFramesAhead);  // rr6-nx
    return !g_running || g_stopping || g_frames_queued - g_frames_done <= ahead;
  });
  --g_done_waiters;
  const uint64_t done = g_frames_done;
  lock.unlock();
  const auto waited = std::chrono::steady_clock::now() - waiting;
  frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count(), 1);  // blur-nx: #37
  // banjo-nx (2026-10-19, banjo_late_input): this frame's slack (the game thread only: one Swap at a time).
  {
    // The game's frame interval (the game thread only), averaged over about 16 frames.
    const uint64_t at = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_last_queue_us && at > g_last_queue_us) {
      const uint64_t gap = std::min<uint64_t>(at - g_last_queue_us, 1'000'000);
      const uint64_t avg = g_frame_interval_us.load(std::memory_order_relaxed);
      g_frame_interval_us.store(avg ? avg + (int64_t(gap) - int64_t(avg)) / 16 : gap, std::memory_order_relaxed);
    }
    g_last_queue_us = at;
  }
  g_slack_us[g_slack_next] =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()) +
      g_applied_delay_us.exchange(0, std::memory_order_relaxed);
  g_slack_next = (g_slack_next + 1) % kSlackFrames;
  g_slack_count = std::min(g_slack_count + 1, kSlackFrames);
  return done;
}

uint64_t FramesDone() {
  std::lock_guard lock(g_mutex);
  return g_frames_done;
}

void Drain() {
  BeforePush();  // vivapinata-nx (2026-11-11): nothing waits on work still held back
  FlushRunIfOwner();  // ssasr-nx (the thread batch F3): this thread's open run published (Drain waits for it)
  DrainSubmit();  // ssasr-nx (2026-11-27, the thread batch T1): everything pushed so far in the render ring first
  std::unique_lock lock(g_mutex);
  if (!g_running || std::this_thread::get_id() == g_thread_id)
    return;
  // dishonored-nx (port 105, dishonored_renut_lockfree_queue): with the ring, the items queued are its claimed positions.
  const uint64_t target = LockfreeOn() ? g_enqueue.load(std::memory_order_acquire) : g_items_queued;
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [target] { return !g_running || g_items_done >= target; });
  --g_done_waiters;
}

WaitScope::WaitScope() : start_(Clock::now()), counted_(t_render_thread) {}

WaitScope::~WaitScope() {
  if (counted_)
    g_waited += Clock::now() - start_;
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running || g_stopping)
      return;
    g_stopping = true;
    g_stop_flag.store(true, std::memory_order_release);  // blur-nx (2026-11-17, audit #11)
  }
  g_accepting.store(false, std::memory_order_release);  // dishonored-nx (port 105): later pushes run inline
  if (AddressWakeOn())
    WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
  g_work_cv.notify_all();
  g_done_cv.notify_all();
  if (g_push_config.submit) {  // ssasr-nx (2026-11-27, the thread batch T1): the worker hands on what it has and ends
    g_sub_stop.store(true, std::memory_order_release);
    WakeSubmit();
  }
}

} // namespace renut::gpu::render_thread
