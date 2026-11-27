/**
 * @file    gpu/backend/queued.cpp
 */
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)
#include "renut_engine/gpu/ssasr_host50.h"  // ssasr-nx (2026-11-26, batch 4)
#include "renut_engine/gpu/backend/latched_switch.h"  // ssasr-nx (2026-11-27, loads batch L107)

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/guest_records.h"  // blur-nx (2026-11-13, #13): records::Lookup
#include "renut_engine/gpu/guest/resources.h"      // blur-nx (2026-11-13, #13): ShaderRecord
#include "renut_engine/gpu/shaders/shader_cache.h"  // blur-nx (2026-11-13, #13): ShaderCacheEntry::floatConstants
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"  // banjo-nx (2026-11-23): TileCount, Flags (banjo_renut_tile_twin_merge)
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"  // blur-nx (2026-11-10, port 76; its key 215): StackHeaderSet
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/gpu/line_compare.h"

REXCVAR_DECLARE(bool, blur_renut_flush_fast_local);  // blur-nx (2026-11-17): defined in guest/command_buffers.cpp

// banjo-nx (2026-10-20, from sa2-nx): see SnapshotKind below.
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas, true, "Banjo/Enhancements",
                    "Renut's renderer: a draw keeps what changed in the device since the device's previous draw (its "
                    "64-byte chunks that differ, and the float constant groups the game marked dirty) instead of a "
                    "copy of the device's first 12,712 bytes (N&B's; SA2 13,088); the render thread rebuilds the device from them. Off: "
                    "the whole copy for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_constants, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): the chain's draws tell the constant uploads which "
                    "float constant groups changed since the previous draw, so only those are compared with what the "
                    "GPU holds (as a recorded command buffer's replayed draws do); a frame's first draw compares them "
                    "all. Off: every draw compares every group its shaders read")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_capture_batches, true, "Banjo/Enhancements",
                    "Renut's renderer (with banjo_renut_capture_pool): the game thread takes its draws' captures from "
                    "the pool, and the render thread gives them back, 32 at a time instead of one lock of the pool's "
                    "per draw on each thread. Off: one at a time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// deadpool-nx (2026-11-17, its checkpoint 8c90b5ca): CapturePool::Give below.
REXCVAR_DEFINE_BOOL(deadpool_capture_binding_reset, true, "Banjo/Enhancements",
                    "deadpool-nx (a fix): a capture given back to the pool forgets its shader binding (halo3-nx's "
                    "banjo_renut_recorded_binds), so one given back without being drawn (a deferred draw replaced before "
                    "its flush, a tile twin's) cannot apply that binding at the next draw, clear or resolve made from it. "
                    "A drawn capture's binding is 0 already. false: kept, the base's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see CapturePool::Keeps.
REXCVAR_DEFINE_BOOL(banjo_renut_small_captures, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_capture_pool): a delta is written into the chain's own scratch and "
                    "copied into its capture at its size (about 250 bytes), instead of into the capture's 12.9 KB "
                    "storage (zero-filled when the capture was made), and the pool keeps up to 8,192 captures or 32 MB "
                    "instead of 2,048. Bangai-O's missile storms queue ~4,250 draws a frame: past 2,048 the pool deleted "
                    "and made again ~2,200 captures a frame. Same data. In-run A/B target 82 (70 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29): see DeltaRuns.
REXCVAR_DEFINE_BOOL(banjo_renut_delta_masks, true, "Banjo/Enhancements",
                    "Renut's renderer on the Switch (banjo_renut_snapshot_deltas): the game thread finds a draw's "
                    "changed device chunks with one NEON reduction per four chunks of a run (the 40 compared chunks are "
                    "three runs), then visits only those (1.1 a draw), instead of a reduction, a move to a register and "
                    "a branch per chunk (65 % of CaptureDelta, 18 % of the game thread in the v4 console storm). The "
                    "same chunks. In-run A/B target 81 (73 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// deadpool-nx (2026-11-17, its checkpoint 8c90b5ca): CopyDeltaChunk, CaptureDelta, DeviceForDraw, PrefetchDeltaLines.
REXCVAR_DEFINE_BOOL(deadpool_delta_copies, true, "Banjo/Enhancements",
                    "deadpool-nx (exact): a delta's short chunk is copied as fixed-size overlapping copies and its "
                    "constant groups 64 bytes at a time (inline, not a call of the run's size), on the game thread and "
                    "the render thread; PrefetchDeltaLines (banjo_renut_delta_prefetch) primes one line in four of the "
                    "guest device's compared runs and none of the shadow, and only when the chain is on this device. "
                    "The same bytes. false: the base's copies and prefetches")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// bangaio-nx (2026-10-29, v6): see PrefetchDeltaLines.
REXCVAR_DEFINE_BOOL(banjo_renut_delta_prefetch, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): a queued draw first asks for the device and shadow "
                    "lines its delta compares (80 lines), so that their cache misses overlap each other and the draw's "
                    "other work instead of stalling the compare (CaptureDelta's samples were its loads in the v5 console "
                    "storm). A hint only: the same data. In-run A/B target 84 (74 in bangaio-nx)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-13, #13 of Blur's docs/cpu-near-exact-50-1113.md, found exact): a Begin*Vertices draw's capture (a
// whole device, queued later than made) copies of the float constants only the registers its two shaders read. The
// render thread reads a whole capture's state for that draw alone (DeviceForDraw), and of the 8 KB of float constants
// only the registers in the bound shaders' cache entries (ShaderCacheEntry::floatConstants: UploadFloatConstants'
// vs_used / ps_used, all of them when the shader indexes the register file); every other byte is copied. A shader
// missing from the cache, or neither a renderer record nor an in-place XGRegister shader: the whole device as before.
// (The first census found none narrowed: every shader of Blur's is an in-place one, which only the render thread's map
// held; the game threads now keep their own, InPlaceShaderEntry.) Blur's console races: ~3,000 such draws a second at 12.9 KB each on the Game Master.
REXCVAR_DEFINE_BOOL(banjo_renut_deferred_narrow, true, "Banjo/Enhancements",
                    "Renut's renderer: a Begin*Vertices draw's device capture copies of the float constants only the "
                    "registers its vertex and pixel shaders read (their cache entries' masks; the whole device when a "
                    "shader has none). Exact: the render thread reads no other register of that capture. Off: the "
                    "whole device, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_deferred_narrow_groups, true, "Banjo/Enhancements",
                    "banjo_renut_deferred_narrow (blur-nx #53): the float constants copied by the 64-byte groups "
                    "(four registers) the shaders read, not up to their highest register. Exact either way")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_deferred_narrow_poison, false, "Banjo/Debug",
                    "banjo_renut_deferred_narrow's check: the float constant registers a narrowed capture leaves out "
                    "are filled with NaNs (a render path that read them would draw visibly wrong)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_narrow, true, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): the game thread compares only the device chunks the "
                    "render thread reads (39 of 71 in N&B, 40 of 77 in SA2: not the dirty masks, ring pointers and call tables before the fetch "
                    "constants, nor the private words, 10880-11775 in N&B (SA2 11008-12223), which only the hooks use on the live "
                    "device). SA2's second console run: comparing all of them was 9 % of its game thread. Off: all")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_neon_compare, true, "Banjo/Enhancements",
                    "Renut's renderer on the Switch: the device chunks of the delta chain and the pushed shared "
                    "constants' lines are compared 64 bytes at a time with NEON (four 128-bit loads a side) instead of "
                    "eight 64-bit words, which GCC left scalar. Off: the 64-bit words")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ssasr-nx (7c0cd28b, the capture pool's churn; renut-ultimate's key): the bytes the capture pool keeps for reuse. At
// its cap it deletes captures, and new ones grow their buffers again on the game thread (a malloc and a free each).
// ssasr-nx's PC census: at 32 MB thousands made and deleted every 10 s; at 96 MB none after warm-up. Mind the heap.
// The user (2026-11-27): 64 MB by default (Beautiful Katamari: 30.5 MB held at 32 with none made, 47.8 MB with the
// submit worker on).
REXCVAR_DEFINE_INT32(ssasr_capture_pool_mb, 64, "Banjo/Enhancements",
                     "The capture pool's byte cap in MB (8-512): 64 (the user's default, 2026-11-27; 32 before); ssasr-nx "
                     "uses 96 (its census: the pool's churn gone). Check the game's heap headroom first (ssasr_renut_host50_census's capture pool line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_snapshot_deltas_verify, false, "Banjo/Enhancements",
                    "Tests (banjo_renut_snapshot_deltas): every draw also keeps the whole copy, and the render thread "
                    "counts the draws whose rebuilt device differs from it in any byte (the 10 s \"[renut] device "
                    "snapshots\" line)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-20, from futari-nx's futari_renut_deferred_chain): see QueueDeferredDraw.
REXCVAR_DEFINE_BOOL(banjo_renut_deferred_chain, false, "Banjo/Enhancements",
                    "Renut's renderer (banjo_renut_snapshot_deltas): a Begin*Vertices draw joins the delta chain too "
                    "instead of a whole device copy (made under the chain's lock and queued under it by the next hook's "
                    "FlushDeferredVertices, before any other draw of the device). OFF by default since package 36q: the "
                    "chain is one for every device, and N&B draws on two (from two threads): a draw of the other device "
                    "made and queued between a Begin*Vertices delta and its queueing puts that delta on the wrong device "
                    "(emulator hangs in 4 of 8 runs with it on, 0 before). Off: a whole copy, as sa2-nx has it. blur-nx (2026-11-13, "
                    "#51): while a Begin*Vertices delta waits for its vertices, a draw of any other device is a whole "
                    "capture outside the chain, so two devices keep the chain's order (Blur draws on one)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): how a queued draw holds its device.
//   Whole:  DrawCapture::state is the device's first kDeviceSnapshotSize bytes (every draw before; the Begin*Vertices
//           draws, which are queued later than they are captured, still).
//   Rebase: the same, and the render thread takes it as its device (the chain's first draw, and each frame's).
//   Delta:  DrawCapture::state is a DeltaHeader and the bytes it names: the render thread applies them to its device.
// banjo-nx: N&B records most of its draws in command buffers, which keep deltas the same way (command_buffers.cpp);
// this is for its immediate draws. SA2 records no command buffers, so every draw was a whole copy: 13 KB a draw, 15-18 MB a frame of City Escape's
// 1,100-1,400 draws written on the game thread, into pooled captures last touched by the render thread on another
// core, then read back by the render thread. A draw changes a few bindings, render states and constants: the deltas are
// those. The chain's order is the queue's: deltas are made and queued under g_delta.mutex, and the render thread runs
// the queue in order.
enum class SnapshotKind : u8 { Whole, Rebase, Delta };

// What a queued draw reads besides guest memory the game keeps until a fence.
// ssasr-nx (2026-11-27, loads batch L122): what every draw writes on the game thread and reads on the render thread first,
// on the record's first three cache lines (the state, the call, the bind, the kind, the narrowed flag, the vertex and
// index storage, the stack headers' count); the clears' and resolves' arguments, the verify copy, the pool's byte count
// and the narrowed device after them. A draw touched five lines of it before (0, 1, 2, the stack headers' at 4, the
// narrowed flag's at 7). The render thread's give-back now writes only the cold line of 'held' (L93: CapturePool).
struct DrawCapture {
  std::vector<u8> state;  // the device, kDeviceSnapshotSize bytes (or a delta: SnapshotKind)
  DrawCall call;
  u32 binding = 0;  // halo3-nx (2026-11-03, banjo_renut_recorded_binds): the shader's bind at the draw (LastBindingOf)
  SnapshotKind snapshot = SnapshotKind::Whole;  // banjo-nx (2026-10-20, from sa2-nx)
  // blur-nx (2026-11-13, #13, banjo_renut_deferred_narrow): 'narrowed' says the draw reads 'narrow' (below) instead of
  // 'state' (cleared when the capture is taken from the pool again: L93).
  bool narrowed = false;
  // ssasr-nx (2026-11-27, the thread batch T1, ssasr_submit_worker): 'state' is staged (the marked chunks raw, or a
  // rebase's device whose shadow copy is still to be made): the submit worker finishes it (FinishStagedCapture) before
  // the render thread sees it, and clears it. In the padding after 'narrowed', on the line the draw writes anyway.
  bool staged = false;
  std::vector<u8> vertices, indices;
  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers): the headers of the stack surfaces bound
  // as targets when this draw, clear or resolve was captured (render/surfaces.h); count 0 nearly always.
  StackHeaderSet stack_headers;
  std::vector<u8> verify;  // banjo-nx (from sa2-nx): the whole device as well (banjo_renut_snapshot_deltas_verify)
  // banjo-nx (2026-10-19, CPU proposal 23): a queued clear's or resolve's arguments. They come from the same pool as
  // the draws' captures: each made three heap allocations on the game thread (its arguments, a device snapshot, a
  // closure too large for std::function's own storage), freed on the render thread through the one malloc lock.
  u32 flags = 0;
  bool has_rect = false, has_color = false, has_dest_format = false;
  ClearRect rect{};
  float color[4] = {};
  float depth = 0.0f;
  u32 stencil = 0;
  u32 dest_texture_va = 0, dest_level = 0, dest_slice = 0;
  i32 dest_x = 0, dest_y = 0;
  d3d::TextureFetchConstant dest_format{};
  u32 resolve_targets[kResolveTargetWords] = {};  // blur-nx (2026-11-13, M-35): a resolve's device words (draw.h)
  float clear_viewport[4] = {};                    // blur-nx (2026-11-13, #55): a clear's viewport (draw.h)
  u32 held = 0;  // bangaio-nx (2026-10-29): the bytes it holds, set when it goes back to the pool (CapturePool::Give)
  // blur-nx (2026-11-13, #13, banjo_renut_deferred_narrow): a narrowed whole capture's device (allocated once per
  // pooled capture, not initialised: a narrowed capture writes every byte the render thread reads).
  std::unique_ptr<u8[]> narrow;
  // blur-nx (2026-11-13, #13's fix): the cache entries whose registers a narrowed capture holds (NarrowCheckLocked).
  const ShaderCacheEntry *narrow_vs = nullptr;
  const ShaderCacheEntry *narrow_ps = nullptr;
  // ssasr-nx (2026-11-27, the thread batch T1): a staged rebase's device (the shadow's place: FinishStagedCapture).
  u32 staged_va = 0;
};

// banjo-nx (banjo_renut_capture_pool): a draw's capture comes from a pool and goes back to it once the render thread
// has drawn it. Each draw made three heap allocations on the game thread (the capture, its 12.7 KB device snapshot,
// the queued closure) and freed them on the render thread: about 2,000 a frame through the Switch's one malloc lock.
// A pooled capture keeps its snapshot's storage, and the closure holds a plain pointer, which std::function stores
// without allocating. Off: every capture is deleted after use, as before (a new one per draw).
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_capture_batches): the pool's lock was taken twice a draw, by the game thread (Take) and
// by the render thread (Give), on two cores: its cache line went back and forth about 1,800 times a City Escape frame.
// Each thread now keeps a few captures of its own and goes to the pool kBatch at a time.
bool CaptureBatchesOn();
bool SmallCapturesOn();
// ssasr-nx (2026-11-27, loads batch M7): banjo_renut_snapshot_deltas_verify, latched at the start (init-only): with it
// off no capture's 'verify' is ever assigned, so it is empty for the whole run and nothing reads, clears or counts it.
// Batch A's L122 put 'verify' on the record's fifth line, which no prefetch fetches: CaptureDelta's verify.clear() on
// every delta capture then missed (22 samples, 0.4 % of the game thread in loadsA, the function's most sampled
// instruction), as did the render thread's empty() test and the give-back's capacity().
bool DeltasVerifyOn();

// ssasr-nx (2026-11-27, the malloc census): captures made new (none in the pool) and deleted at a give (the pool full).
std::atomic<uint64_t> g_pool_new{0}, g_pool_deleted{0};
class CapturePool {
public:
  // bangaio-nx (2026-10-29): room for the most the pool keeps, so that no push under the lock reallocates.
  CapturePool() {
    free_.reserve(kMaxFreeSmall);
    free_held_.reserve(kMaxFreeSmall);
    big_.reserve(kMaxFreeSmall);
    big_held_.reserve(kMaxFreeSmall);
  }

  // ssasr-nx (2026-11-27, batch 5 N23): a capture for a whole device, a narrowed one or a Begin*Vertices draw's vertices
  // ('big'), from the captures that already hold buffers that size. The console's census (H27): about 200 system
  // allocations a second on the game thread, nearly all of them a pooled capture growing: a whole snapshot (12,888
  // bytes), a narrowed device's, or the deferred vertices (9-12 KB) into a capture that had held a delta of a few hundred
  // bytes (the pool handed out its captures in any order), each a malloc walking newlib's bins (_malloc_r 0.16-0.19 ms
  // a frame) and the old buffer's free on the render thread. The pool now keeps such captures apart and gives them to the
  // draws that need them; the rest as before. Only where buffers live: the captures' contents are the same.
  // ssasr-nx (2026-11-27, the malloc follow-up's second fix; the PC census: a rebase still grew one in three of its
  // captures): the big ones are two kinds, a whole device's state and a Begin*Vertices draw's vertices, and the last
  // one was often the other kind. 'need' picks one of the right kind from the last eight when there is one.
  enum class BigNeed { kAny, kState, kVertices };
  DrawCapture *TakeBig(BigNeed need = BigNeed::kAny) {
    if (::ssasr::host50::On(::ssasr::host50::N(23))) {
      std::lock_guard lock(mutex_);
      if (!big_.empty()) {
        if (need != BigNeed::kAny) {
          const size_t last = big_.size() - 1;
          for (size_t k = 0, i = big_.size(); k < 8 && i > 0; ++k) {
            --i;
            const DrawCapture &c = *big_[i];
            if (need == BigNeed::kState ? c.state.capacity() >= d3d::kDeviceSnapshotSize
                                        : c.vertices.capacity() >= 4096) {
              if (i != last) {
                std::swap(big_[i], big_[last]);
                std::swap(big_held_[i], big_held_[last]);
              }
              break;
            }
          }
        }
        DrawCapture *capture = big_.back();
        big_.pop_back();
        DropHeldLocked(big_held_, 1);
        Fresh(*capture);  // L93
        return capture;
      }
    }
    return Take();
  }

  // banjo-nx (2026-10-20, from sa2-nx): 'chain_locked' when the caller holds g_delta.mutex (the chain's draws): its batch is a plain
  // list that lock guards. Thread-local lists cost a call a use here (this toolchain's TLS goes through
  // __aarch64_read_tp: 0.8 % of the render thread in the second console run).
  DrawCapture *Take(bool chain_locked = false) {
    if (CaptureBatchesOn() && chain_locked) {
      std::vector<DrawCapture *> &taken = chain_taken_;
      if (taken.empty()) {
        std::lock_guard lock(mutex_);
        const size_t n = std::min(kBatch, free_.size());
        taken.assign(free_.end() - std::ptrdiff_t(n), free_.end());
        free_.resize(free_.size() - n);
        DropHeldLocked(free_held_, n);
        if (n == 0 && !big_.empty()) {  // ssasr-nx (batch 5 N23): the big ones rather than new captures
          const size_t m = std::min(kBatch, big_.size());
          taken.assign(big_.end() - std::ptrdiff_t(m), big_.end());
          big_.resize(big_.size() - m);
          DropHeldLocked(big_held_, m);
        }
      }
      if (!taken.empty()) {
        DrawCapture *capture = taken.back();
        taken.pop_back();
        Fresh(*capture);  // L93
        // bangaio-nx (2026-10-29, ARM): the next draw's capture, last written by the render thread on another core, is
        // fetched for writing while the game runs to that draw (its first lines read in CaptureDelta were a miss each
        // draw in the v4 console profile).
        if (!taken.empty()) {
          const char *next = reinterpret_cast<const char *>(taken.back());
          __builtin_prefetch(next, 1);
          __builtin_prefetch(next + 64, 1);
          // (sasrt-nx's C8 fetched +256 too, the stack headers' count: it is on the third line now, L122)
          __builtin_prefetch(next + 128, 1);
        }
        return capture;
      }
      g_pool_new.fetch_add(1, std::memory_order_relaxed);  // ssasr-nx (the malloc census)
      return new DrawCapture();
    }
    {
      std::lock_guard lock(mutex_);
      if (!free_.empty()) {
        DrawCapture *capture = free_.back();
        free_.pop_back();
        DropHeldLocked(free_held_, 1);
        Fresh(*capture);  // L93
        return capture;
      }
    }
    g_pool_new.fetch_add(1, std::memory_order_relaxed);  // ssasr-nx (the malloc census)
    return new DrawCapture();
  }
  // banjo-nx (2026-10-20, from sa2-nx): 'render_thread' from the render thread's items (only that thread then touches its batch; a
  // queue run inline at shutdown has the render thread gone).
  // bangaio-nx (2026-10-29): the captures the pool does not keep are deleted after its lock is let go (the frees take
  // the malloc lock, which the game thread's allocations wait on).
  void Give(DrawCapture *capture, bool render_thread = false) {
    if (!capture)
      return;
    if (banjo::renut_gpu::CapturePoolOn()) {
      if (!render_thread) {  // ssasr-nx (loads batch L93): the render thread's are emptied when taken again (Fresh)
        capture->vertices.clear();
        capture->indices.clear();
        capture->narrowed = false;  // blur-nx (2026-11-13, #13)
      }
      static const bool binding_reset = REXCVAR_GET(deadpool_capture_binding_reset);
      if (binding_reset)
        capture->binding = 0;  // deadpool-nx: an undrawn capture's LastBindingOf answer
      capture->held = HeldBytes(*capture);  // its lines are this thread's now: not read again under the lock
      if (CaptureBatchesOn() && render_thread) {
        std::vector<DrawCapture *> &given = render_given_;
        given.push_back(capture);
        if (given.size() < kBatch)
          return;
        const bool small = SmallCapturesOn();  // its guard and the A/B word read once, not under the lock a capture
        {
          std::lock_guard lock(mutex_);
          for (DrawCapture *&c : given) {
            if (KeepLocked(c, small))
              c = nullptr;
          }
        }
        for (DrawCapture *c : given) {
          if (c)
            g_pool_deleted.fetch_add(1, std::memory_order_relaxed);  // ssasr-nx (the malloc census)
          delete c;
        }
        given.clear();
        return;
      }
      const bool small = SmallCapturesOn();
      {
        std::lock_guard lock(mutex_);
        if (KeepLocked(capture, small))
          return;
      }
    }
    g_pool_deleted.fetch_add(1, std::memory_order_relaxed);  // ssasr-nx (the malloc census)
    delete capture;
  }
  size_t HeldBytesNow() const { return held_; }  // ssasr-nx (the malloc census; read without the lock)

private:
  // bangaio-nx (2026-10-29, banjo_renut_small_captures, A/B 82 here): the console's missile storm queued ~4,250 draws a frame.
  // At 2,048 the pool deleted the rest when the render thread gave them back, and the game thread made ~2,200 again a
  // frame: each a malloc, and a 12.9 KB zero-fill of its delta storage, under the one malloc lock that the render
  // thread's frees took too. CaptureDelta now copies a delta into its capture at its size, so a capture keeps a few
  // hundred bytes, and the pool keeps up to kMaxFreeSmall. Whole copies (rebases, clears, resolves, deltas off) keep
  // 12.9 KB each: the byte cap holds them to about as many as before (32 MB: 2,048 of those were 26 MB).
  static constexpr size_t kMaxFree = 2048;        // more than a frame's draws in flight (banjo-nx: ~2,000 a frame)
  static constexpr size_t kMaxFreeSmall = 8192;   // bangaio-nx: twice the storm's
  // ssasr-nx (2026-11-27, the malloc follow-up's third fix): 96 MB, was 32. The PC census: the pool at 28-31 MB of the
  // 32 in five reports of six, making and deleting 3,700-4,700 captures a report (each new one empty, grown again on the
  // game thread). The console's heap at its race peak: 1,244 MB in use of 3,030 (14 MB of it free in 83,568 pieces:
  // why each malloc there walks so long).
  // renut-ultimate: the cap from ssasr_capture_pool_mb (64 by default; 32 before; ssasr-nx 96).
  static size_t MaxHeldBytes() {
    static const size_t v = size_t(std::clamp(int(REXCVAR_GET(ssasr_capture_pool_mb)), 8, 512)) << 20;
    return v;
  }
  static constexpr size_t kBatch = 32;      // banjo-nx (from sa2-nx): captures a thread takes or gives back at once

  static u32 HeldBytes(const DrawCapture &c) {
    return u32(sizeof(DrawCapture) + c.state.capacity() + c.vertices.capacity() + c.indices.capacity() +
               (DeltasVerifyOn() ? c.verify.capacity() : 0) + (c.narrow ? d3d::kDeviceSnapshotSize : 0));  // M7
  }
  // Under mutex_; 'small': SmallCapturesOn().
  bool KeepLocked(DrawCapture *c, bool small) {
    // ssasr-nx (2026-11-27, batch 5 N23): a capture holding a big buffer to big_ (its bytes count toward the same cap).
    const bool big = ::ssasr::host50::On(::ssasr::host50::N(23)) && IsBig(*c);
    std::vector<DrawCapture *> &list = big ? big_ : free_;
    std::vector<u32> &held = big ? big_held_ : free_held_;
    const size_t count = free_.size() + big_.size();
    if (small ? count >= kMaxFreeSmall || held_ + c->held > MaxHeldBytes() : count >= kMaxFree)
      return false;
    list.push_back(c);
    held.push_back(c->held);
    held_ += c->held;
    return true;
  }
  // ssasr-nx (2026-11-27, loads batch L93): a capture leaving the pool emptied of the last draw's vertices and narrowed
  // flag (on the game thread, which writes these lines next anyway; the render thread's give-back left them as it read
  // them). Each written only when not already so.
  static void Fresh(DrawCapture &c) {
    if (!c.vertices.empty())
      c.vertices.clear();
    if (!c.indices.empty())
      c.indices.clear();
    if (c.narrowed)
      c.narrowed = false;
  }
  // N23: a capture that holds a whole device's state, a narrowed device or a Begin*Vertices draw's vertices.
  static bool IsBig(const DrawCapture &c) {
    return c.state.capacity() >= d3d::kDeviceSnapshotSize || c.narrow || c.vertices.capacity() >= 4096;
  }
  // Under mutex_: the last n captures' bytes, which left their list, from its byte list (contiguous, so no capture is
  // read).
  void DropHeldLocked(std::vector<u32> &held, size_t n) {
    const size_t end = held.size();
    size_t bytes = 0;
    for (size_t i = end - n; i < end; ++i)
      bytes += held[i];
    held.resize(end - n);
    held_ -= bytes;
  }

  std::mutex mutex_;
  std::vector<DrawCapture *> free_;
  std::vector<u32> free_held_;  // bangaio-nx (2026-10-29): free_'s captures' bytes, in the same order
  std::vector<DrawCapture *> big_;  // ssasr-nx (batch 5 N23): the captures that hold big buffers
  std::vector<u32> big_held_;       // their bytes, in the same order
  size_t held_ = 0;             // their sum
  std::vector<DrawCapture *> chain_taken_;   // banjo-nx (from sa2-nx): under g_delta.mutex
  std::vector<DrawCapture *> render_given_;  // banjo-nx (from sa2-nx): the render thread's
};
CapturePool g_capture_pool;

// ssasr-nx (2026-11-27, the malloc follow-up: _malloc_r 0.26 ms a frame of the game thread with the submit worker on,
// 0.20 before it): at each place the game thread writes a capture's buffer, how often the buffer had to grow (a malloc,
// and the old buffer's free), in the census's snapshot line (ssasr_renut_host50_census).
enum GrowSite : u32 {
  kGrowWhole,
  kGrowRebase,
  kGrowMarked,
  kGrowStaged,
  kGrowDelta,
  kGrowUpVertices,
  kGrowUpIndices,
  kGrowDeferred,
  kGrowSites
};
std::atomic<uint64_t> g_grow_uses[kGrowSites], g_grow_count[kGrowSites], g_grow_bytes[kGrowSites];
[[gnu::always_inline]] inline void NoteGrow(GrowSite site, size_t capacity, size_t need) {
  if (::ssasr::host50::CensusOn()) [[unlikely]] {
    g_grow_uses[site].fetch_add(1, std::memory_order_relaxed);
    if (need > capacity) {
      g_grow_count[site].fetch_add(1, std::memory_order_relaxed);
      g_grow_bytes[site].fetch_add(need, std::memory_order_relaxed);
    }
  }
}
// ssasr-nx (2026-11-27, the malloc follow-up's fix; the PC census: thousands of growths in ten seconds, a vector grown
// by resize / assign gets exactly the size asked for, so the next draw a little bigger on the same pooled capture grew
// it again, each a malloc walking newlib's bins and a free): a buffer that must grow grows to the size rounded up to
// 'step', so a capture that grew once seldom grows again. Only capacity: the bytes written are the same.
[[gnu::always_inline]] inline void GrowWithRoom(std::vector<u8> &v, size_t need, size_t step) {
  if (need > v.capacity()) [[unlikely]]
    v.reserve((need + step - 1) / step * step);
}
// ssasr-nx (2026-11-27, N18's census again, after T1; port 388, with the port's map in place of ssasr-nx's two sets):
// the game thread's staged copies: raw chunks a staged draw, the draws a state tag made compare the 17 render-state
// chunks, the draws whose state tags were word 2's alone (the only ones a map can narrow) and how often each of word
// 2's tags came in them (the candidates for a map), and of those the draws whose tags are all in
// ssasr_tags_narrow_map, with the chunks the narrowed sets would have copied instead.
std::atomic<uint64_t> g_n18_draws{0}, g_n18_raw_chunks{0}, g_n18_state_tagged{0}, g_n18_states_alone{0},
    g_n18_in_map{0}, g_n18_chunks_now{0}, g_n18_chunks_narrow{0};
std::atomic<uint64_t> g_n18_tag_draws[64];
std::string N18Report() {
  const uint64_t draws = g_n18_draws.exchange(0, std::memory_order_relaxed);
  const uint64_t raw = g_n18_raw_chunks.exchange(0, std::memory_order_relaxed);
  const uint64_t tagged = g_n18_state_tagged.exchange(0, std::memory_order_relaxed);
  const uint64_t alone = g_n18_states_alone.exchange(0, std::memory_order_relaxed);
  const uint64_t in_map = g_n18_in_map.exchange(0, std::memory_order_relaxed);
  const uint64_t now = g_n18_chunks_now.exchange(0, std::memory_order_relaxed);
  const uint64_t narrow = g_n18_chunks_narrow.exchange(0, std::memory_order_relaxed);
  uint64_t per_tag[64];
  for (int t = 0; t < 64; ++t) per_tag[t] = g_n18_tag_draws[t].exchange(0, std::memory_order_relaxed);
  std::string top;  // the four most frequent of word 2's tags in those draws
  for (int k = 0; k < 4; ++k) {
    int best = -1;
    for (int t = 0; t < 64; ++t)
      if (per_tag[t] && (best < 0 || per_tag[t] > per_tag[best])) best = t;
    if (best < 0) break;
    top += fmt::format("{}2.{} {:.0f} %", top.empty() ? "" : ", ", best,
                       alone ? 100.0 * double(per_tag[best]) / double(alone) : 0.0);
    per_tag[best] = 0;
  }
  return fmt::format("; N18 census: {} staged draws, {:.1f} raw chunks each; {} with a state tag ({:.0f} %), {} of them "
                     "word 2's alone (tags: {}); {} with the map's tags alone ({:.0f} % of the tagged): their compare "
                     "sets {:.1f} chunks, narrowed {:.1f}",
                     draws, draws ? double(raw) / double(draws) : 0.0, tagged,
                     draws ? 100.0 * double(tagged) / double(draws) : 0.0, alone, top.empty() ? "none" : top, in_map,
                     tagged ? 100.0 * double(in_map) / double(tagged) : 0.0, in_map ? double(now) / double(in_map) : 0.0,
                     in_map ? double(narrow) / double(in_map) : 0.0);
}
std::string GrowReport() {
  static const char *const kNames[kGrowSites] = {"whole", "rebase", "marked", "staged", "delta", "UP vertices",
                                                 "UP indices", "deferred vertices"};
  std::string out = fmt::format("; capture pool (census): {} made, {} deleted, {:.1f} MB held",
                                g_pool_new.exchange(0, std::memory_order_relaxed),
                                g_pool_deleted.exchange(0, std::memory_order_relaxed),
                                double(g_capture_pool.HeldBytesNow()) / 1048576.0);
  out += "; capture buffers grown on the game thread (census):";
  for (u32 i = 0; i < kGrowSites; ++i) {
    const uint64_t uses = g_grow_uses[i].exchange(0, std::memory_order_relaxed);
    const uint64_t n = g_grow_count[i].exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = g_grow_bytes[i].exchange(0, std::memory_order_relaxed);
    if (uses)
      out += fmt::format(" {} {} of {} ({:.0f} bytes each)", kNames[i], n, uses, n ? double(bytes) / double(n) : 0.0);
  }
  return out;
}
// ssasr-nx (2026-11-27, batch 5 N23): QueueDeferredDraw's CaptureDelta takes a big capture (under the chain's lock).
bool g_capture_big_next = false;

// A Begin*Vertices draw waiting for its vertices, per device.
// ssasr-nx (2026-11-26, batch 4 #13, ssasr_renut_host50): the chain's lock as one atomic word. std::mutex was
// pthread_mutex_lock -> newlib -> libnx mutexLock and back again at every draw (the lock functions had 0.17 ms a frame
// of the game thread on the console, rc10b). Uncontended: an exchange (acquire) and a store (release). Contended (a
// second thread drawing, a command buffer thread forgetting the chain): spins, then sleeps 50 us at a time, so a
// holder of lower priority on the same core gets to run (a yield would not let it). Item 13 off: the std::mutex.
class ChainMutex {
public:
  void lock() {
    if (!::ssasr::host50::On(13)) [[unlikely]] {
      fallback_.lock();
      return;
    }
    if (!flag_.exchange(true, std::memory_order_acquire)) [[likely]]
      return;
    LockSlow();
  }
  void unlock() {
    if (!::ssasr::host50::On(13)) [[unlikely]] {
      fallback_.unlock();
      return;
    }
    flag_.store(false, std::memory_order_release);
  }

private:
  [[gnu::noinline, gnu::cold]] void LockSlow() {
    for (u32 n = 0;; ++n) {
      while (flag_.load(std::memory_order_relaxed)) {
        if (++n > 64)
          std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (!flag_.exchange(true, std::memory_order_acquire))
        return;
    }
  }
  std::atomic<bool> flag_{false};
  std::mutex fallback_;
};

struct DeferredDraw {
  DrawCapture *capture = nullptr;  // owned (g_capture_pool)
  const u8 *scratch = nullptr;
  u32 bytes = 0;
  const u8 *index_scratch = nullptr;
  u32 index_bytes = 0;
};
ChainMutex g_deferred_mutex;  // ssasr-nx (2026-11-27, batch 5 H81): one word, as the chain's (H13)
// ssasr-nx (2026-11-26, batch 4 #31, ssasr_renut_host50): the waiting draws by device as a short vector (a device or two
// wait at once), not an unordered_map, which made and freed a node at every Begin*Vertices draw (the hash table's
// functions and the allocator on the game thread's profile). The map's interface as QueueDeferredDraw and
// FlushDeferredVertices use it (find / end / erase / count / [] / size); the same entries either way.
class DeferredMap {
public:
  struct Entry {
    u32 first = 0;
    DeferredDraw second;
  };
  DeferredMap() { entries_.reserve(16); }
  Entry *find(u32 key) {
    if (!::ssasr::host50::On(31))
      return MapFind(key);
    for (Entry &e : entries_)
      if (e.first == key)
        return &e;
    return nullptr;
  }
  Entry *end() { return nullptr; }
  void erase(Entry *e) {
    if (!::ssasr::host50::On(31)) {
      map_.erase(e->first);
      return;
    }
    *e = entries_.back();
    entries_.pop_back();
  }
  size_t count(u32 key) { return find(key) ? 1 : 0; }
  DeferredDraw &operator[](u32 key) {
    if (!::ssasr::host50::On(31))
      return map_[key];
    if (Entry *e = find(key))
      return e->second;
    entries_.push_back({key, {}});
    return entries_.back().second;
  }
  size_t size() const { return ::ssasr::host50::On(31) ? entries_.size() : map_.size(); }

private:
  Entry *MapFind(u32 key) {
    const auto it = map_.find(key);
    if (it == map_.end())
      return nullptr;
    map_entry_ = {it->first, it->second};
    return &map_entry_;
  }
  std::vector<Entry> entries_;
  std::unordered_map<u32, DeferredDraw> map_;  // item 31 off: as before
  Entry map_entry_;
};
DeferredMap g_deferred;
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_callbacks_fast): g_deferred.size(), kept under g_deferred_mutex and read without it
// (FlushDeferredVertices: every hook calls it first). A Begin*Vertices draw is flushed by the next hook of its device,
// on the thread that made it, which sees the count in program order.
std::atomic<size_t> g_deferred_count{0};
// blur-nx (2026-11-17, the 24-hour run; blur_renut_flush_fast_local): the same count per device, hashed (a collision
// only sends a hook through the locks), so one device's hooks are not sent through the chain's lock and the deferred
// map's by another device's Begin*Vertices draw. Changed under g_deferred_mutex with the map; read without it by the
// thread that flushes its own device, as the count above.
constexpr u32 kDeferredSlots = 64;
std::atomic<u32> g_deferred_by_device[kDeferredSlots];
inline std::atomic<u32> &DeferredOf(u32 device_va) { return g_deferred_by_device[(device_va * 2654435761u) >> 26]; }
// blur-nx (2026-11-13, #52's census): FlushDeferredVertices' copies, for the snapshot report.
std::atomic<uint64_t> g_deferred_vertex_bytes{0}, g_deferred_vertex_draws{0};

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): the delta chain's layout. Of the device's first kDeviceSnapshotSize
// bytes, the float constants (VertexShaderF, PixelShaderF: 8 KB, two thirds) go by the groups m_Pending marks dirty
// (one bit per four registers, bit 63 - group, as the recorded command buffers keep them: command_buffers.cpp); the
// rest goes by 64-byte chunks (30 before the constants, 47 after them, the last one 32 bytes), those that differ from
// the chain's previous draw (compared on the game thread with its shadow).
constexpr u32 kDeltaVertexF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderF));
constexpr u32 kDeltaPixelF = u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, PixelShaderF));
constexpr u32 kDeltaTailBegin = kDeltaPixelF + 256 * 16;
constexpr u32 kDeltaUnitBytes = 64;  // a chunk, and a constant group
constexpr u32 kDeltaHeadChunks = kDeltaVertexF / kDeltaUnitBytes;
constexpr u32 kDeltaTailBytes = d3d::kDeviceSnapshotSize - kDeltaTailBegin;
constexpr u32 kDeltaChunks = kDeltaHeadChunks + (kDeltaTailBytes + kDeltaUnitBytes - 1) / kDeltaUnitBytes;
static_assert(kDeltaVertexF % kDeltaUnitBytes == 0 && kDeltaPixelF == kDeltaVertexF + 256 * 16);
static_assert(kDeltaTailBegin == u32(offsetof(d3d::Device, m_Constants) + offsetof(d3d::Constants, VertexShaderB)));
static_assert(kDeltaChunks <= 128);
static_assert(kDeltaHeadChunks == kChainHeadChunks && kDeltaTailBegin == kChainTailBegin);  // draw.h's ChainHint

constexpr u32 DeltaChunkOffset(u32 i) {
  return i < kDeltaHeadChunks ? i * kDeltaUnitBytes : kDeltaTailBegin + (i - kDeltaHeadChunks) * kDeltaUnitBytes;
}
constexpr u32 DeltaChunkSize(u32 i) {
  return i < kDeltaHeadChunks ? kDeltaUnitBytes
                              : std::min(kDeltaUnitBytes, kDeltaTailBytes - (i - kDeltaHeadChunks) * kDeltaUnitBytes);
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas_narrow): the device bytes the render thread never reads from a draw's
// device (DrawStateLocked and what it calls: the accessors of render/draw.cpp, textures.cpp, surfaces.cpp, the pipeline
// key): before the fetch constants (the dirty masks, ring pointers and Set/Get*State call tables, which a recorded
// command buffer's replay leaves zero too), and the private words 11008-12223 (the creation thread, the fences, the
// swap flags: the hooks read and write them on the live device). Their chunks are not compared; the render thread's
// device keeps them as the frame's whole copy had them.
constexpr u32 kDeltaUnreadEnd = kDeltaVertexF - 0x300;  // 0x480: the fetch constants' start
// banjo-nx (2026-10-20): N&B's private block lies 128 bytes earlier than SA2's (d3d.h: the creation thread at
// 10888, the fence tags at 10908/10928, the swap flags at 10941), and its vertex declaration at 11820 (SA2 12240).
// The render thread's last packet read is m_MiscPacket (d3d_native_state.cpp, up to 0x2A50); nothing it reads lies
// between 0x2A50 and the vertex declaration. So 10880 (0x2A80, the first chunk past m_MiscPacket) to 11776 (the
// chunk before the one holding the vertex declaration). SA2: 11008-12224.
// katamari-nx: a 64-byte chunk boundary, not a field: 11776 is still the last boundary before Beautiful Katamari's
// vertex declaration (+11812), and the words between are private ones the render thread does not read.
constexpr u32 kDeltaPrivateUnreadBegin = 10880, kDeltaPrivateUnreadEnd = 11776;
constexpr bool DeltaChunkRead(u32 i) {
  const u32 at = DeltaChunkOffset(i);
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at + 64 <= kDeltaPrivateUnreadEnd));
}
static_assert(kDeltaUnreadEnd == 0x480 && kDeltaUnreadEnd % kDeltaUnitBytes == 0);
static_assert((kDeltaPrivateUnreadBegin - kDeltaTailBegin) % kDeltaUnitBytes == 0 &&
              (kDeltaPrivateUnreadEnd - kDeltaTailBegin) % kDeltaUnitBytes == 0);
static_assert(d3d::kDeviceOffsetVertexDeclaration >= kDeltaPrivateUnreadEnd);
// The chunks compared: all, or the read ones.
struct DeltaChunkList {
  u32 count = 0;
  u8 index[128] = {};
};
constexpr DeltaChunkList MakeDeltaChunkList(bool narrow) {
  DeltaChunkList l{};
  for (u32 i = 0; i < kDeltaChunks; ++i)
    if (!narrow || DeltaChunkRead(i))
      l.index[l.count++] = u8(i);
  return l;
}
constexpr DeltaChunkList kDeltaAllChunks = MakeDeltaChunkList(false);
constexpr DeltaChunkList kDeltaReadChunks = MakeDeltaChunkList(true);
static_assert(kDeltaReadChunks.count == 39);  // N&B (SA2 40; bangaio-nx's XDK 20209: 40)

// bangaio-nx (2026-10-29, banjo_renut_delta_masks, A/B 81 here, 73 in bangaio-nx): a chunk list as runs of whole chunks at consecutive
// offsets, and each short chunk (the tail's last, 24 bytes in XDK 20209) as a run of its own. CaptureDelta asks
// Lines64DifferMask which chunks of a run differ (line_compare.h: one NEON reduction per four chunks and no branch per
// chunk), then visits the set bits in order: the chunks the per-chunk loop found, in its order. The narrow list is
// three runs (12, 12 and 15 chunks) and the short chunk.
struct DeltaRun {
  u8 first = 0;  // chunk index
  u8 count = 0;  // chunks (at most 32: Lines64DifferMask's)
  bool short_chunk = false;
};
struct DeltaRuns {
  u32 count = 0;
  DeltaRun run[16] = {};
};
constexpr DeltaRuns MakeDeltaRuns(const DeltaChunkList &l) {
  DeltaRuns r{};
  for (u32 k = 0; k < l.count; ++k) {
    const u32 i = l.index[k];
    const bool whole = DeltaChunkSize(i) == kDeltaUnitBytes;
    if (whole && r.count) {
      DeltaRun &last = r.run[r.count - 1];
      if (!last.short_chunk && last.count < 32 && last.first + last.count == i &&
          DeltaChunkOffset(i) == DeltaChunkOffset(last.first) + last.count * kDeltaUnitBytes) {
        ++last.count;
        continue;
      }
    }
    r.run[r.count++] = DeltaRun{u8(i), u8(1), !whole};
  }
  return r;
}
// A short chunk (fewer than 64 bytes): 8 bytes at a time, inline (memcmp with a size known only at run time was a call).
inline bool ShortChunkDiffers(const u8 *a, const u8 *b, u32 size) {
  uint64_t diff = 0;
  u32 k = 0;
  for (; k + 8 <= size; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  for (; k < size; ++k)
    diff |= u8(a[k] ^ b[k]);
  return diff != 0;
}
// deadpool-nx (deadpool_delta_copies): a short chunk (under 64 bytes) as fixed-size copies, the last overlapping the one
// before it, which the compiler keeps inline (a variable-size memcpy is a call).
inline void ShortChunkCopy(u8 *dst, const u8 *src, u32 size) {
  if (size >= 16) {
    u32 k = 0;
    for (; k + 16 <= size; k += 16)
      std::memcpy(dst + k, src + k, 16);
    if (k < size)
      std::memcpy(dst + size - 16, src + size - 16, 16);
    return;
  }
  if (size >= 8) {
    std::memcpy(dst, src, 8);
    std::memcpy(dst + size - 8, src + size - 8, 8);
    return;
  }
  if (size >= 4) {
    std::memcpy(dst, src, 4);
    std::memcpy(dst + size - 4, src + size - 4, 4);
    return;
  }
  for (u32 k = 0; k < size; ++k)
    dst[k] = src[k];
}
inline bool DeltaCopiesOn() {
  static const bool on = REXCVAR_GET(deadpool_delta_copies);
  return on;
}
constexpr DeltaRuns kDeltaAllRuns = MakeDeltaRuns(kDeltaAllChunks);
constexpr DeltaRuns kDeltaReadRuns = MakeDeltaRuns(kDeltaReadChunks);
// renut-ultimate: bangaio-nx asserted its XDK's layout (4 runs, the last the short chunk); the runs are made from whatever the
// chunk lists are, so only their number is bounded here.
static_assert(kDeltaReadRuns.count <= 16 && kDeltaAllRuns.count <= 16);
// Whether device byte 'at' is one the render thread reads (the verify mode compares those).
constexpr bool DeltaByteRead(u32 at) {
  return !(at < kDeltaUnreadEnd || (at >= kDeltaPrivateUnreadBegin && at < kDeltaPrivateUnreadEnd));
}

struct DeltaHeader {
  uint64_t chunks[2];  // chunk i follows: bit i % 64 of word i / 64
  uint64_t vs, ps;     // the float constant groups that follow (bit 63 - group), the vertex shader's first
};
// The most a delta holds: every chunk and every group.
constexpr size_t kDeltaMaxBytes = sizeof(DeltaHeader) + d3d::kDeviceSnapshotSize;

// Calls f(first, count) for each run of consecutive groups in an m_Pending mask (bit 63 - group).
template <typename F>
void ForEachGroupRun(uint64_t mask, F f) {
  while (mask) {
    const u32 first = u32(std::countl_zero(mask));
    const u32 count = u32(std::countl_one(mask << first));
    f(first, count);
    mask = first + count >= 64 ? 0 : mask & (~uint64_t(0) >> (first + count));
  }
}

// Whether two whole chunks differ: fixed-size loads the compiler keeps inline and vectorises.
bool DeltaChunkDiffers(const u8 *a, const u8 *b) {
  uint64_t diff = 0;
  for (u32 k = 0; k < kDeltaUnitBytes; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

// The game threads' side of the chain.
struct DeltaChain {
  ChainMutex mutex;                     // the chain's draws are made and queued in one order
  // The device it follows; none: the next draw rebases. banjo-nx (2026-10-26): atomic so ForgetSnapshotDeltas can
  // tell without the lock that a recorded op's device is not the chain's; written under the lock only.
  std::atomic<const d3d::Device *> device{nullptr};
  uint64_t vs = 0, ps = 0;              // groups whole captures consumed since its last draw: they follow
  uint64_t deltas = 0, rebases = 0, bytes = 0, chunks = 0, groups = 0;  // the 10 s report's
  uint64_t whole = 0;  // banjo-nx (from sa2-nx): whole captures outside the chain (Begin*Vertices draws), under the lock too
  uint64_t narrowed = 0, narrowed_bytes = 0;  // blur-nx (2026-11-13, #13): of those, narrowed, and the bytes they copied
  uint64_t narrowed_prefix_bytes = 0;         // ... what #13's prefixes alone would have copied (#53 / #54's census)
  // blur-nx (2026-11-13, #51, banjo_renut_deferred_chain): the device whose Begin*Vertices delta of the chain waits for
  // its vertices (0: none). While one waits, a draw of any other device is a whole capture outside the chain: queued
  // before that delta, it would have been applied ahead of it on the render thread (N&B's hangs with the chain on and
  // two devices drawing from two threads), so the chain's order stays its queue order however many devices draw.
  // 'cross': the draws made whole that way (the 10 s report's).
  u32 deferred_device = 0;
  uint64_t cross = 0;
  // The device at its last draw (the chunks). ssasr-nx (2026-11-27, batch 5 H32): a pointer into a buffer 16 KB
  // longer, placed at each rebase (PlaceDeltaShadow) so that its line of each offset falls in another L1 set than the
  // device's own (the A57's L1 data cache: 32 KB, two ways of 16 KB): the compare loads both lines of an offset.
  alignas(64) u8 shadow_storage[d3d::kDeviceSnapshotSize + 16384];
  u8 *shadow = shadow_storage;
  // bangaio-nx (2026-10-29, banjo_renut_small_captures): each delta is written here, then copied into its capture at its
  // size. The same lines every draw: they stay in the game thread's cache.
  alignas(64) u8 scratch[kDeltaMaxBytes];
};
DeltaChain g_delta;

// The render thread's side: the device the chain's draws rebuild.
alignas(64) u8 g_delta_device[d3d::kDeviceSnapshotSize];
std::atomic<uint64_t> g_delta_verified{0}, g_delta_differ{0};

// blur-nx (2026-11-13, M-31 of docs/cpu-moonshots-50-1113.md and its audit: the census first). The devices that draw and
// the threads they draw from, since the start (at most four of each; more counted apart): banjo_renut_deferred_chain is
// off by default because one chain serves every device and N&B drew on two devices from two threads. A thread notes
// itself and a device only when its device changes, so a draw pays one thread-local compare.
std::atomic<uint64_t> g_census_devices[4]{}, g_census_threads[4]{};
std::atomic<uint32_t> g_census_more_devices{0}, g_census_more_threads{0};
void CensusNote(std::atomic<uint64_t> (&set)[4], std::atomic<uint32_t> &more, uint64_t value) {
  for (auto &slot : set) {
    uint64_t seen = slot.load(std::memory_order_relaxed);
    if (seen == value)
      return;
    if (seen == 0 && (slot.compare_exchange_strong(seen, value, std::memory_order_relaxed) || seen == value))
      return;
  }
  more.fetch_add(1, std::memory_order_relaxed);
}
// ssasr-nx (2026-11-26, batch 4 #22): the last drawer a plain global (a thread_local is a call on the Switch:
// __aarch64_read_tp, at every draw). A census: two threads drawing in turn only note more often.
u32 g_note_last_device = 0;
void NoteDrawerSlow(u32 device_va);
// ssasr-nx (2026-11-27, batch 5 H17): the census's same-device test inline at the draw (a call at every draw before:
// 0.03-0.04 ms a frame), the rest out of line.
[[gnu::always_inline]] inline void NoteDrawer(u32 device_va) {
  if (::ssasr::host50::On(22) && g_note_last_device == device_va)
    return;
  NoteDrawerSlow(device_va);
}
void NoteDrawerSlow(u32 device_va) {
  if (::ssasr::host50::On(22)) {
    if (g_note_last_device == device_va)
      return;
    g_note_last_device = device_va;
    static thread_local char tag;  // (its address: one per thread; read only when the device changes)
    CensusNote(g_census_threads, g_census_more_threads, uint64_t(reinterpret_cast<uintptr_t>(&tag)));
    CensusNote(g_census_devices, g_census_more_devices, device_va);
    return;
  }
  thread_local u32 last_device = 0;
  if (last_device == device_va)
    return;
  if (last_device == 0) {
    static thread_local char tag;  // (its address: one per thread)
    CensusNote(g_census_threads, g_census_more_threads, uint64_t(reinterpret_cast<uintptr_t>(&tag)));
  }
  last_device = device_va;
  CensusNote(g_census_devices, g_census_more_devices, device_va);
}
std::string DrawerCensus() {
  std::string devices;
  uint32_t n_devices = 0, n_threads = 0;
  for (auto &slot : g_census_devices)
    if (const uint64_t v = slot.load(std::memory_order_relaxed)) {
      devices += fmt::format("{}{:08X}", n_devices ? " " : "", uint32_t(v));
      ++n_devices;
    }
  for (auto &slot : g_census_threads)
    n_threads += slot.load(std::memory_order_relaxed) != 0;
  return fmt::format("; drawing since the start: {} devices ({}{}), {} threads{}", n_devices, devices,
                     g_census_more_devices.load(std::memory_order_relaxed) ? " and more" : "", n_threads,
                     g_census_more_threads.load(std::memory_order_relaxed) ? " and more" : "");
}
// banjo_renut_snapshot_deltas_constants: the chain as a replay of the constant uploads' tracking (draw.h,
// ReplayConstants): its id (a new one at each whole copy; the top bit keeps it apart from the command buffers'
// replays, numbered from 1) and the float constant groups written since the upload last consumed them.
// banjo_renut_chain_reuse: the number of the chain's last draw (draw.h ChainHint::seq). banjo-nx (2026-10-20): from the
// shared NextChainSeq, since the command buffers' replays give hints too: when a draw of theirs came between two of
// the chain's (the number is not this one + 1), the chain's next draw marks every chunk changed.
// ssasr-nx (2026-11-27, batch 5 N9/N10): the render thread's (DeviceForDraw and the replays write them at every delta)
// on cache lines of their own. The linker had put the dirty masks on the line of d3d_native.h's g_chain_tags_apart_slot
// and g_chain_prefetch_slot (read at every setter's mark on the game thread), and the id, count and number on the line
// of g_note_last_device (read at every draw): the console's 5B profile, 5 and 4 samples on those loads.
struct alignas(64) RenderChainState {
  uint64_t id = 0, count = 0;
  uint64_t vs_dirty = ~uint64_t(0), ps_dirty = ~uint64_t(0);
  uint64_t seq = 0;
};
RenderChainState g_render_chain;
uint64_t &g_chain_id = g_render_chain.id, &g_chain_count = g_render_chain.count;
uint64_t &g_chain_vs_dirty = g_render_chain.vs_dirty, &g_chain_ps_dirty = g_render_chain.ps_dirty;
uint64_t &g_chain_seq = g_render_chain.seq;

// ssasr-nx (2026-11-27, loads batch L107): latched without a guarded static (latched_switch.h), as bangaio-nx did for
// the others below
bool DeltasOn() {
  static std::atomic<int8_t> on{0};
  return LatchedSwitch(on, [] { return bool(REXCVAR_GET(banjo_renut_snapshot_deltas)); });
}
bool ChainConstantsOn() {
  static std::atomic<int8_t> on{0};
  return LatchedSwitch(on, [] { return DeltasOn() && REXCVAR_GET(banjo_renut_snapshot_deltas_constants); });
}
// bangaio-nx (2026-10-29, ARM): these read the latched switches (banjo_renut_tuning.h, plain loads), not function
// statics: each static's guard was a load-acquire at every draw on the game thread (the v4 console profile).
bool CaptureBatchesOn() {
  return banjo::renut_gpu::Latch().capture_batches;
}
bool SmallCapturesOn() {
  return banjo::renut_gpu::Latch().small_captures && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 82;  // 70 in bangaio-nx
}
bool NeonCompareOn() {
  return banjo::renut_gpu::Latch().neon_compare;
}
[[gnu::always_inline]] inline bool DeltasNarrowOn() {  // ssasr-nx (batch 5 H6): inline, as MarkedDeltasOn
  return banjo::renut_gpu::Latch().deltas_narrow;
}
bool DeltaMasksOn() {  // bangaio-nx (2026-10-29, A/B 81 here)
  return banjo::renut_gpu::Latch().delta_masks && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 81;  // 73 in bangaio-nx
}
bool DeltaPrefetchOn() {  // bangaio-nx (2026-10-29, v6, A/B 74)
  return banjo::renut_gpu::Latch().delta_prefetch && banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) != 84;  // 74 in bangaio-nx
}
// banjo-nx (2026-10-20, from futari-nx): banjo_renut_deferred_chain.
bool DeferredChainOn() {
  static std::atomic<int8_t> on{0};  // ssasr-nx (loads batch L107)
  return LatchedSwitch(on, [] { return bool(REXCVAR_GET(banjo_renut_deferred_chain)); });
}
bool DeltasVerifyOn() {
  return banjo::renut_gpu::Latch().deltas_verify;  // bangaio-nx (2026-10-29): latched (with the deltas on)
}

// The original draw calls consume the device's dirty masks as they write
// their packets; a snapshot taken after this has them clear, as the command
// buffer ops' do.
void ClearPending(d3d::Device *device, u32 device_va) {
  // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas): the dirty constant groups a whole capture consumes go with the chain's next
  // delta (its callers hold g_delta.mutex when the deltas are on).
  if (device == g_delta.device.load(std::memory_order_relaxed)) {
    g_delta.vs |= uint64_t(device->m_Pending[0]);
    g_delta.ps |= uint64_t(device->m_Pending[1]);
  }
  // ssasr-nx (2026-11-26, batch 4 #2, the PC verify race's 4 misses of chunk +0x4C0): the dirty tags it clears stay
  // marked in the device's slot, so the chain's next delta (MarkedDelta) still compares what they stood for: the
  // Begin*Vertices draws' whole captures (a fifth of the race's draws on the PC) came between two of the chain's.
  // ssasr-nx (2026-11-27, the thread batch F1's fix: the PC guard race's chunk +0x5C0): the chunks those tags stand for
  // were written by the game's own code, which the setter log never sees: copied raw at the chain's next delta.
  if (::ssasr::host50::On(2) && device_va) {
    const u32 slot = native::TouchSlotOf(device_va);
    const uint64_t bits =
        native::TagChunks(uint64_t(device->m_Pending[2]), uint64_t(device->m_Pending[3]), uint64_t(device->m_Pending[4]));
    native::g_touch[slot].chunks |= bits;
    if (slot == native::g_log_slot) [[unlikely]]
      native::g_log_raw |= bits;
  }
  for (auto &mask : device->m_Pending)
    mask = 0;
}

// banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas), on the render thread: the device a queued draw reads.
const u8 *DeviceForDraw(const DrawCapture &capture) {
  if (capture.snapshot == SnapshotKind::Whole) {
    // banjo-nx (2026-10-28, per-draw night, A/B 62): a number of its own, every chunk and field changed (as a replay's
    // first draw): the fast path may take it after the draw before it. The chain's own number stays (its next delta
    // draw is not consecutive with it: every chunk then too). Off half: not a chain draw, as before.
    if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 62) {
      g_chain_hint = {};  // not a chain draw (banjo_renut_chain_reuse)
    } else {
      g_chain_hint.seq = NextChainSeq();
      g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);
      g_chain_hint.fields = kFieldsAll;
    }
    return capture.narrowed ? capture.narrow.get() : capture.state.data();  // blur-nx (2026-11-13, #13)
  }
  if (capture.snapshot == SnapshotKind::Rebase) {
    std::memcpy(g_delta_device, capture.state.data(), d3d::kDeviceSnapshotSize);
    g_chain_id = (uint64_t(1) << 63) | ++g_chain_count;  // its first draw compares every group
    g_chain_vs_dirty = g_chain_ps_dirty = ~uint64_t(0);
    g_chain_hint.seq = g_chain_seq = NextChainSeq();
    g_chain_hint.changed[0] = g_chain_hint.changed[1] = ~uint64_t(0);  // a whole copy: every chunk
    g_chain_hint.fields = kFieldsAll;
  } else {
    const u8 *from = capture.state.data();
    DeltaHeader h;
    std::memcpy(&h, from, sizeof(h));
    from += sizeof(h);
    const uint64_t seq = NextChainSeq();
    const bool consecutive = seq == g_chain_seq + 1;  // banjo-nx: no replayed draw in between
    g_chain_hint.seq = g_chain_seq = seq;
    g_chain_hint.changed[0] = consecutive ? h.chunks[0] : ~uint64_t(0);
    g_chain_hint.changed[1] = consecutive ? h.chunks[1] : ~uint64_t(0);
    g_chain_hint.fields = consecutive ? ChainFields(h.chunks) : kFieldsAll;  // banjo-nx (2026-10-26)
    g_chain_vs_dirty |= h.vs;
    g_chain_ps_dirty |= h.ps;
    for (u32 word = 0; word < 2; ++word) {
      for (uint64_t bits = h.chunks[word]; bits; bits &= bits - 1) {
        const u32 i = word * 64 + u32(std::countr_zero(bits));
        const u32 size = DeltaChunkSize(i);
        // Whole chunks as a fixed-size copy, which stays inline.
        if (size == kDeltaUnitBytes)
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, kDeltaUnitBytes);
        else if (DeltaCopiesOn())
          ShortChunkCopy(g_delta_device + DeltaChunkOffset(i), from, size);  // deadpool-nx
        else
          std::memcpy(g_delta_device + DeltaChunkOffset(i), from, size);
        from += size;
      }
    }
    for (const auto [dirty, base] : {std::pair{h.vs, kDeltaVertexF}, std::pair{h.ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        if (DeltaCopiesOn()) {  // deadpool-nx: a fixed-size copy a group
          u8 *dst = g_delta_device + base + first * kDeltaUnitBytes;
          for (u32 k = 0; k < count; ++k, dst += kDeltaUnitBytes, from += kDeltaUnitBytes)
            std::memcpy(dst, from, kDeltaUnitBytes);
          return;
        }
        const u32 bytes = count * kDeltaUnitBytes;
        std::memcpy(g_delta_device + base + first * kDeltaUnitBytes, from, bytes);
        from += bytes;
      });
    }
  }
  if (DeltasVerifyOn() && !capture.verify.empty()) {  // M7: the switch first (a latched flag), then the record's line
    g_delta_verified.fetch_add(1, std::memory_order_relaxed);
    // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas_narrow): the bytes the render thread reads; with the narrow compare off, all.
    bool same;
    if (DeltasNarrowOn()) {
      same = std::memcmp(g_delta_device + kDeltaUnreadEnd, capture.verify.data() + kDeltaUnreadEnd,
                         kDeltaPrivateUnreadBegin - kDeltaUnreadEnd) == 0 &&
             std::memcmp(g_delta_device + kDeltaPrivateUnreadEnd, capture.verify.data() + kDeltaPrivateUnreadEnd,
                         d3d::kDeviceSnapshotSize - kDeltaPrivateUnreadEnd) == 0;
    } else {
      same = std::memcmp(g_delta_device, capture.verify.data(), d3d::kDeviceSnapshotSize) == 0;
    }
    if (!same) {
      g_delta_differ.fetch_add(1, std::memory_order_relaxed);
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        u32 at = 0;
        while (at < d3d::kDeviceSnapshotSize &&
               (g_delta_device[at] == capture.verify[at] || (DeltasNarrowOn() && !DeltaByteRead(at))))
          ++at;
        RGPU_WARN("Device snapshot delta: the rebuilt device differs from the device at +{} ({} draw): {:02X}, the "
                  "device {:02X} (banjo_renut_snapshot_deltas_verify)",
                  at, capture.snapshot == SnapshotKind::Rebase ? "a whole" : "a delta", g_delta_device[at],
                  capture.verify[at]);
      }
    }
  }
  return g_delta_device;
}

// blur-nx (2026-11-13, #13's fix): a narrowed capture's shaders as the render thread finds them for its draw. One whose
// cache entry is not the one the capture was narrowed for (the guest re-registering a shader in the memory of one a
// queued draw uses) reads registers the capture may have left out: counted, and reported every 10 s.
std::atomic<uint64_t> g_narrow_checked{0}, g_narrow_differ{0};

void NarrowCheckLocked(const DrawCapture &capture, const u8 *device) {
  auto word = [device](u32 offset) {
    u32 v;
    std::memcpy(&v, device + offset, 4);
    return __builtin_bswap32(v);
  };
  const ShaderRecord *vs = FindShaderLocked(word(d3d::kDeviceOffsetVertexShader), RecordKind::VertexShader);
  const ShaderRecord *ps = FindShaderLocked(word(d3d::kDeviceOffsetPixelShader), RecordKind::PixelShader);
  // A shader the draw cannot use (none found, or no cache entry: GetHostShader gives none) reads no register.
  const bool vs_differs = vs && vs->entry && vs->entry != capture.narrow_vs;
  const bool ps_differs = ps && ps->entry && ps->entry != capture.narrow_ps;
  g_narrow_checked.fetch_add(1, std::memory_order_relaxed);
  if (vs_differs || ps_differs)
    g_narrow_differ.fetch_add(1, std::memory_order_relaxed);
}

// ssasr-nx (2026-11-26, batch 4 #1): the render thread's item for a queued draw, a plain function (QueuePrefetchedPlain).
[[gnu::hot]] void RunCaptureItem(void *arg) {
  DrawCapture *capture = static_cast<DrawCapture *>(arg);
  DrawCall call = capture->call;
  if (!capture->vertices.empty())
    call.up_vertices = capture->vertices.data();
  if (!capture->indices.empty())
    call.up_indices = capture->indices.data();
  const u8 *device = DeviceForDraw(*capture);
  if (capture->narrowed)  // blur-nx (2026-11-13, #13's fix)
    NarrowCheckLocked(*capture, device);
  banjo::renut_gpu::CountDrawSource(0);
  // blur-nx (2026-11-10, port 76; its key 215)
  SetReplayStackHeaders(capture->stack_headers.count ? &capture->stack_headers : nullptr);
  if (capture->binding) {  // halo3-nx (2026-11-03): after a replay's, this draw's own
    ApplyBindingLocked(capture->binding);
    capture->binding = 0;
  }
  if (capture->snapshot != SnapshotKind::Whole && ChainConstantsOn()) {
    // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas_constants): every constant is the rebuilt device's (no running device).
    const ReplayConstants chain{nullptr, ~uint64_t(0), ~uint64_t(0), &g_chain_vs_dirty, &g_chain_ps_dirty,
                                g_chain_id};
    DrawStateLocked(device, call, &chain);
  } else {
    DrawStateLocked(device, call);
  }
  g_chain_hint = {};  // banjo-nx (from sa2-nx): only for the draw itself
  SetReplayStackHeaders(nullptr);  // blur-nx (2026-11-10, port 76; its key 215)
  g_capture_pool.Give(capture, true);
}

void FinishStagedCapture(void *arg);  // ssasr-nx (2026-11-27, the thread batch T1): below, with CaptureDelta
bool StagingOn();                    // (the same)

[[gnu::hot]] void QueueCapture(DrawCapture *capture) {
  // ssasr-nx (2026-11-27, the thread batch T1): a staged capture is finished on the submit worker first.
  if (capture->staged) {
    render_thread::QueuePrefetchedPlainPre(capture, capture->state.data(), &RunCaptureItem, capture,
                                           &FinishStagedCapture);
    return;
  }
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): the capture and its device bytes, written on the game thread's core,
  // are fetched into the render thread's cache while it does the item before this one.
  render_thread::QueuePrefetchedPlain(capture, capture->narrowed ? capture->narrow.get() : capture->state.data(),
                                      &RunCaptureItem, capture);
}
// ssasr-nx (2026-11-27, batch 5 N15): QueueDraw's captures (CaptureDelta's and Capture's) are never narrowed (only
// CaptureDeferred narrows, and the pool's Give clears the flag), so their device is 'state': the flag's line, last
// written by the render thread in Give, is not read (the console's 4d and 5B profiles: 28 and 32 samples on that load,
// 0.66 % of the game thread).
[[gnu::hot]] void QueueWholeOrDelta(DrawCapture *capture) {
  if (capture->staged) {  // ssasr-nx (2026-11-27, the thread batch T1)
    render_thread::QueuePrefetchedPlainPre(capture, capture->state.data(), &RunCaptureItem, capture,
                                           &FinishStagedCapture);
    return;
  }
  render_thread::QueuePrefetchedPlain(capture, capture->state.data(), &RunCaptureItem, capture);
}

// ---------------------------------------------------------------------------
// banjo-nx (2026-11-23, the Nutty Acres GPU work): banjo_renut_tile_twin_merge (render/draw.cpp says what is drawn)
// ---------------------------------------------------------------------------
// On the game thread: a tessellated draw predicated to one tile that is not the last (N&B's sea, sub_82386BF8: the
// upper tile's draw) is held back instead of queued. The next draw, when it is the same call for another single tile
// of the same device, goes with it as one item; anything else first queues the held one alone: another draw (here,
// under the chain's lock, before it is queued), any other queue or wait (render_thread's before-push hook), so the
// render thread gets the draws and everything else in the order the game made them. The pair's item rebuilds both
// devices from the chain, draws the first for both tiles when DrawStateLocked can, and else draws the two as before.
struct HeldTwin {
  DrawCapture *capture = nullptr;
  u32 device_va = 0;
  int tile = -1;
};
std::mutex g_twin_mutex;
HeldTwin g_twin_held;                    // under g_twin_mutex
std::atomic<bool> g_twin_holding{false};  // a capture is held (the hook's check without the lock)
thread_local bool t_twin_queueing = false;  // this thread queues under g_twin_mutex: its hook call has nothing to do
std::atomic<uint64_t> g_twin_pairs_queued{0};
// banjo-nx (2026-11-23, the first console run): the holds and how each ended (the 10 s twin line, TwinQueueCensus).
std::atomic<uint64_t> g_twin_holds{0}, g_twin_flush_hook{0}, g_twin_flush_draw{0}, g_twin_flush_draw_tess{0};

// The one tile the device's draws are predicated to while tiling (vivapinata_tiles_once's test), or -1.
int TwinTileOf(u32 device_va, u32 &tiles) {
  const u8 *device = GuestPtr<const u8>(device_va);
  if (!device)
    return -1;
  tiles = *reinterpret_cast<const be_u32 *>(device + native::off::TileCount);
  if (!(device[native::off::Flags] & 0x20) || tiles < 2 || tiles > 16)
    return -1;
  const u32 mask = *reinterpret_cast<const be_u32 *>(device + d3d::kDeviceOffsetPredication);
  if (!mask)
    return -1;
  int tile = -1;
  for (u32 i = 0; i < tiles; ++i) {
    if (!(mask & (3u << (2 * i))))
      continue;
    if (tile >= 0)
      return -1;
    tile = int(i);
  }
  return tile;
}

// The render thread's item for a held draw and its twin.
void QueueTwinPair(DrawCapture *a, DrawCapture *b) {
  g_twin_pairs_queued.fetch_add(1, std::memory_order_relaxed);
  render_thread::QueuePrefetched(a, a->narrowed ? a->narrow.get() : a->state.data(), [a, b] {
    // The first's device kept: the chain's device moves on to the second's.
    static std::array<u8, d3d::kDeviceSnapshotSize> image;  // render thread
    const u8 *device_a = DeviceForDraw(*a);
    if (a->narrowed)
      NarrowCheckLocked(*a, device_a);
    std::memcpy(image.data(), device_a, image.size());
    const u8 *device_b = DeviceForDraw(*b);
    if (b->narrowed)
      NarrowCheckLocked(*b, device_b);
    banjo::renut_gpu::CountDrawSource(0);
    banjo::renut_gpu::CountDrawSource(0);
    const auto draw = [](DrawCapture *c, const u8 *device) {
      // No chain hint: the second's delta is in the chain's device already, so the first is not the next of the chain's
      // draws, and the second's draw follows a draw of another device image (the reuses then make everything again).
      g_chain_hint = {};
      SetReplayStackHeaders(c->stack_headers.count ? &c->stack_headers : nullptr);
      if (c->binding) {
        ApplyBindingLocked(c->binding);
        c->binding = 0;
      }
      if (c->snapshot != SnapshotKind::Whole && ChainConstantsOn()) {
        const ReplayConstants chain{nullptr, ~uint64_t(0), ~uint64_t(0), &g_chain_vs_dirty, &g_chain_ps_dirty,
                                    g_chain_id};
        DrawStateLocked(device, c->call, &chain);
      } else {
        DrawStateLocked(device, c->call);
      }
      // The constant uploads compare every group at the next draw: the first's draw took the groups the second's delta
      // wrote with the first's values.
      g_chain_vs_dirty = g_chain_ps_dirty = ~uint64_t(0);
      g_chain_hint = {};
      SetReplayStackHeaders(nullptr);
    };
    g_tile_twin.device_b = device_b;
    g_tile_twin.call_b = b->call;
    g_tile_twin.active = true;
    g_tile_twin.merged = false;
    draw(a, image.data());
    const bool merged = g_tile_twin.merged;
    g_tile_twin = {};
    if (!merged)
      draw(b, device_b);
    g_capture_pool.Give(a, true);
    g_capture_pool.Give(b, true);
  });
}

// The held draw queued alone (render_thread's before-push hook: before anything else is queued or waited for).
void FlushHeldTwin() {
  // ssasr-nx (2026-11-27, batch 5 H13): the held flag first (a load), the thread's flag (a call on the Switch,
  // __aarch64_read_tp) only when a draw is held: this hook runs before every push of the race.
  if (!g_twin_holding.load(std::memory_order_acquire) || t_twin_queueing)
    return;
  std::lock_guard lock(g_twin_mutex);
  if (!g_twin_held.capture)
    return;
  DrawCapture *held = g_twin_held.capture;
  g_twin_held = {};
  g_twin_holding.store(false, std::memory_order_release);
  g_twin_flush_hook.fetch_add(1, std::memory_order_relaxed);
  t_twin_queueing = true;
  QueueCapture(held);
  t_twin_queueing = false;
}

// QueueDraw's capture (made under the chain's lock): true when it was taken (held, or queued with the held one).
bool TakeTwinCapture(u32 device_va, const DrawCall &call, DrawCapture *capture) {
  static std::atomic<int8_t> on_latched{0};  // ssasr-nx (loads batch L107): its guard was 6 samples in loadsA
  const bool on = LatchedSwitch(on_latched, [] {
    // ssasr-nx (2026-11-27, the thread batch T9): with staging no capture is held (below), so the hook is not set
    // (FlushHeldTwin ran before every push of the race: 0.02 ms a frame on the game thread).
    const bool v = TileTwinMergeOn() && DeltasOn() && !StagingOn();
    if (v)
      render_thread::SetBeforePushHook2(&FlushHeldTwin);
    return v;
  });
  if (!on)
    return false;
  // ssasr-nx (2026-11-27, the thread batch T1): a staged capture is not held (the pair's item has no finish on the
  // submit worker); it goes as an unpaired draw, the way a draw no twin follows goes. (No draw of this race is ever
  // held: batch 5's H44; with staging on, none is.)
  if (capture->staged)
    return false;
  u32 tiles = 0;
  // A/B 204 (banjo-nx's 320, numbered below 256 here: port 188; banjo_renut_ab_target): the off half holds nothing (a draw held when it starts goes alone).
  const int tile = call.tessellation && !call.up_vertices && !call.up_indices && capture->vertices.empty() &&
                           capture->indices.empty() && banjo::renut_gpu::g_ab_off_plain != 204
                       ? TwinTileOf(device_va, tiles)
                       : -1;
  if (tile < 0 && !g_twin_holding.load(std::memory_order_acquire))
    return false;
  // A tile run vivapinata_tiles_once holds (queued before this draw) goes first: at most one thing held at a time.
  if (tile >= 0) {
    t_twin_queueing = true;
    render_thread::RunBeforePushHook(1);
    t_twin_queueing = false;
  }
  std::lock_guard lock(g_twin_mutex);
  t_twin_queueing = true;
  if (DrawCapture *held = g_twin_held.capture) {
    const HeldTwin h = g_twin_held;
    g_twin_held = {};
    g_twin_holding.store(false, std::memory_order_release);
    const DrawCall &c = held->call;
    if (tile >= 0 && h.device_va == device_va && h.tile != tile && c.primitive == call.primitive &&
        c.indexed == call.indexed && c.start_vertex == call.start_vertex && c.count == call.count &&
        c.base_vertex == call.base_vertex && c.start_index == call.start_index && c.tessellation) {
      QueueTwinPair(held, capture);
      t_twin_queueing = false;
      return true;
    }
    g_twin_flush_draw.fetch_add(1, std::memory_order_relaxed);
    if (call.tessellation)
      g_twin_flush_draw_tess.fetch_add(1, std::memory_order_relaxed);
    QueueCapture(held);
  }
  if (tile >= 0 && u32(tile) + 1 < tiles) {
    g_twin_holds.fetch_add(1, std::memory_order_relaxed);
    g_twin_held = {capture, device_va, tile};
    g_twin_holding.store(true, std::memory_order_release);
    t_twin_queueing = false;
    return true;
  }
  t_twin_queueing = false;
  return false;
}

// ssasr-nx (2026-11-26, batch 4 #35): the per-draw functions are [[gnu::hot]] (GCC keeps them together in .text.hot);
// the whole capture, outside the chain, is rare in a race.
[[gnu::cold]] DrawCapture *Capture(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  ClearPending(device, device_va);
  DrawCapture *capture = g_capture_pool.TakeBig(CapturePool::BigNeed::kState);  // ssasr-nx (batch 5 N23): a capture holding a whole device
  // assign() keeps a pooled capture's storage (Snapshot() made a new vector each time).
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  NoteGrow(kGrowWhole, capture->state.capacity(), d3d::kDeviceSnapshotSize);  // ssasr-nx (the malloc census)
  capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  capture->snapshot = SnapshotKind::Whole;  // banjo-nx (from sa2-nx)
  CaptureStackHeaders(bytes, capture->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  if (DeltasVerifyOn())  // M7: off, never assigned this run
    capture->verify.clear();
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  return capture;
}

// blur-nx (2026-11-13, #13, banjo_renut_deferred_narrow): the float constant registers the bound shader at 'offset' reads,
// as a count from register 0 (its highest one + 1); false when it is neither a renderer record of 'kind' nor an in-place
// shader of 'kind' (FindShaderLocked's order: the record registry, then the in-place shaders), or has no cache entry.
// 'none_ok': a null shader reads none (the pixel shader: the render thread then uses kNoRegisters). 'out': the entry.
bool ShaderRegisters(const u8 *device, u32 offset, RecordKind kind, bool none_ok, u32 &count, uint64_t &groups,
                     const ShaderCacheEntry *&out) {
  const u32 va = (u32(device[offset]) << 24) | (u32(device[offset + 1]) << 16) | (u32(device[offset + 2]) << 8) |
                 u32(device[offset + 3]);
  groups = 0;
  out = nullptr;
  if (!va) {
    count = 0;
    return none_ok;
  }
  RecordKind found;
  const ShaderCacheEntry *entry = nullptr;
  if (records::Lookup(va, &found) && found == kind) {
    const auto *record = GuestPtr<const ShaderRecord>(va);
    if (!record)
      return false;
    entry = record->entry;
  } else if (!InPlaceShaderEntry(va, kind, &entry)) {  // blur-nx (2026-11-13, #13's fix): Blur's shaders
    return false;
  }
  if (!entry)
    return false;
  out = entry;
  // ssasr-nx (2026-11-27, batch 5 H10): the count and groups kept for the last entry of each kind, with its read mask
  // (recomputed when either differs): two lookups a Begin*Vertices draw, the same shaders draw after draw (0.08-0.09 ms
  // a frame in the console tables). Under the chain's lock (CaptureDeferred's callers hold it when the deltas are on).
  struct Memo {
    const ShaderCacheEntry *entry = nullptr;
    uint32_t mask[8] = {};
    u32 count = 0;
    uint64_t groups = 0;
  };
  static Memo memo[2];
  const bool memo_on = ::ssasr::host50::On(::ssasr::host50::H(10)) && DeltasOn();
  Memo &m = memo[kind == RecordKind::PixelShader ? 1 : 0];
  if (memo_on && m.entry == entry && std::memcmp(m.mask, entry->floatConstants, sizeof(m.mask)) == 0) {
    count = m.count;
    groups = m.groups;
    return true;
  }
  count = 0;
  for (int w = 7; w >= 0; --w) {
    if (const u32 bits = entry->floatConstants[w]) {
      count = u32(w) * 32 + 32 - u32(__builtin_clz(bits));
      break;
    }
  }
  // blur-nx (2026-11-13, #53): the 64-byte groups (four registers) holding a register it reads.
  for (u32 w = 0; w < 8; ++w) {
    const u32 bits = entry->floatConstants[w];
    for (u32 g = 0; g < 8; ++g)
      if ((bits >> (4 * g)) & 0xFu) groups |= uint64_t(1) << (w * 8 + g);
  }
  if (memo_on) {  // H10
    m.entry = entry;
    std::memcpy(m.mask, entry->floatConstants, sizeof(m.mask));
    m.count = count;
    m.groups = groups;
  }
  return true;
}

// blur-nx (2026-11-13, #53): the groups of 'groups' (bit g = registers 4g..4g+3, 64 bytes) copied in runs; the bytes.
size_t CopyGroups(u8 *out, const u8 *bytes, u32 base, uint64_t groups) {
  size_t copied = 0;
  while (groups) {
    const u32 first = u32(__builtin_ctzll(groups));
    const uint64_t rest = groups >> first;
    const u32 run = rest == ~uint64_t(0) >> first ? 64 - first : u32(__builtin_ctzll(~rest));
    std::memcpy(out + base + first * 64, bytes + base + first * 64, size_t(run) * 64);
    copied += size_t(run) * 64;
    groups = first + run >= 64 ? 0 : groups & ~((uint64_t(1) << (first + run)) - 1);
  }
  return copied;
}

// The capture of a Begin*Vertices draw: Capture()'s, with only the float constant registers its shaders read (the
// rest of the device whole); Capture() itself when a shader's registers are not known.
DrawCapture *CaptureDeferred(u32 device_va, const DrawCall &call) {
  static const bool narrow_on = REXCVAR_GET(banjo_renut_deferred_narrow);
  if (!narrow_on)
    return Capture(device_va, call);
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  u32 vs_regs = 0, ps_regs = 0;
  uint64_t vs_groups = 0, ps_groups = 0;
  const ShaderCacheEntry *vs_entry = nullptr, *ps_entry = nullptr;
  if (!ShaderRegisters(bytes, d3d::kDeviceOffsetVertexShader, RecordKind::VertexShader, false, vs_regs, vs_groups,
                       vs_entry) ||
      !ShaderRegisters(bytes, d3d::kDeviceOffsetPixelShader, RecordKind::PixelShader, true, ps_regs, ps_groups,
                       ps_entry))
    return Capture(device_va, call);
  ClearPending(device, device_va);
  DrawCapture *capture = g_capture_pool.TakeBig();  // ssasr-nx (batch 5 N23): its narrow buffer kept
  capture->narrow_vs = vs_entry;  // blur-nx (2026-11-13, #13's fix): checked on the render thread (NarrowCheckLocked)
  capture->narrow_ps = ps_entry;
  if (!capture->narrow)
    capture->narrow.reset(new u8[d3d::kDeviceSnapshotSize]);
  u8 *out = capture->narrow.get();
  static const bool poison = REXCVAR_GET(banjo_renut_deferred_narrow_poison);
  if (poison)  // NaNs (0xFF bytes) in every byte left out
    std::memset(out, 0xFF, d3d::kDeviceSnapshotSize);
  // blur-nx (2026-11-13, #54): the device's never-read ranges left out too (the delta chain's narrow map, which
  // banjo_renut_snapshot_deltas_verify checks: the dirty masks, ring pointers and call tables before the fetch
  // constants, the private words 10880-11775); banjo_renut_deferred_narrow_groups: #53's groups, else the prefixes.
  static const bool groups_on = REXCVAR_GET(banjo_renut_deferred_narrow_groups);
  size_t copied = 0;
  std::memcpy(out + kDeltaUnreadEnd, bytes + kDeltaUnreadEnd, kDeltaVertexF - kDeltaUnreadEnd);
  copied += kDeltaVertexF - kDeltaUnreadEnd;
  if (groups_on) {
    copied += CopyGroups(out, bytes, kDeltaVertexF, vs_groups);
    copied += CopyGroups(out, bytes, kDeltaPixelF, ps_groups);
  } else {
    std::memcpy(out + kDeltaVertexF, bytes + kDeltaVertexF, size_t(vs_regs) * 16);
    std::memcpy(out + kDeltaPixelF, bytes + kDeltaPixelF, size_t(ps_regs) * 16);
    copied += size_t(vs_regs + ps_regs) * 16;
  }
  std::memcpy(out + kDeltaTailBegin, bytes + kDeltaTailBegin, kDeltaPrivateUnreadBegin - kDeltaTailBegin);
  std::memcpy(out + kDeltaPrivateUnreadEnd, bytes + kDeltaPrivateUnreadEnd,
              d3d::kDeviceSnapshotSize - kDeltaPrivateUnreadEnd);
  copied += (kDeltaPrivateUnreadBegin - kDeltaTailBegin) + (d3d::kDeviceSnapshotSize - kDeltaPrivateUnreadEnd);
  capture->narrowed = true;
  capture->snapshot = SnapshotKind::Whole;
  CaptureStackHeaders(bytes, capture->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  if (DeltasVerifyOn())  // M7: off, never assigned this run
    capture->verify.clear();
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  if (DeltasOn()) {  // (the caller holds g_delta.mutex then)
    ++g_delta.narrowed;
    g_delta.narrowed_bytes += copied;
    g_delta.narrowed_prefix_bytes += d3d::kDeviceSnapshotSize - size_t(512 - vs_regs - ps_regs) * 16;  // #13's alone
  }
  return capture;
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): a draw of the chain. Requires g_delta.mutex, held until the capture
// is queued.
// ssasr-nx (2026-11-26, batch 4; the user: "Do all 50 in current batch please"; gpu/ssasr_host50.h): the delta from
// the marks. Every device write the native setters make marks its chunk (d3d_native.h Touch: a new value only, the
// compare-on-set), in the device's touch slot: for the drawing device (no slot of its own) a sink nobody else reads.
// Every write the game's own inline code makes sets the field's dirty tag (m_Pending words 2-4), which TagChunks turns
// into the chunks the tag may stand for. CaptureDelta compared all 40 read chunks with its shadow at every draw (0.89
// ms a frame on the console, rc10b; ~3.9 of them changed). Now:
//  #2:  only the tagged chunks are compared; the marked ones
//  #45: are taken without a compare (a setter marked them for a new value; taking one changed back is harmless);
//  #3:  a draw with nothing marked, tagged or dirty is a header alone;
//  #5:  (row 12) a dirty float constant group the same as the shadow's is left out (72 % of the groups the game wrote
//       in the race were the same as the ones last pushed); the shadow now follows the groups too;
//  #6:  the delta written into the capture at its size (no scratch, no second copy), its groups 64 bytes at a time
//       (constant-size copies: no C library memcpy call, which had 0.6 ms of the game thread);
//  #43: one path with no per-chunk switch (the old one stays for every other case).
// The marks' completeness: ssasr_renut_host50_verify compares every chunk as well at every delta and counts what the
// marks missed (taking it, so the picture stays right); without it, every ssasr_renut_host50_mark_check deltas (64)
// one is checked so, and a miss turns this path off for the rest of the run (logged), as banjo_renut_tags_check does
// for the recording devices. banjo_renut_snapshot_deltas_verify checks the rebuilt device as before.
constexpr u32 kTouchFetchBits = (native::kTouchFetchEnd - native::kTouchFetchBegin) / kDeltaUnitBytes;  // 12
static_assert(native::kTouchFetchBegin == kDeltaUnreadEnd && native::kTouchFetchEnd == kDeltaVertexF &&
              native::kTouchTailBegin == kDeltaTailBegin);
// A touch bit's delta chunk (d3d_native.h MakeTouchTable: the fetch constants' 12 chunks, then the tail's).
constexpr u32 TouchBitChunk(u32 b) {
  return b < kTouchFetchBits ? kDeltaUnreadEnd / kDeltaUnitBytes + b : kDeltaHeadChunks + (b - kTouchFetchBits);
}
constexpr uint64_t MakeReadTouchBits() {
  uint64_t m = 0;
  for (u32 b = 0; b < 64; ++b) {
    const u32 i = TouchBitChunk(b);
    if (i < kDeltaChunks && DeltaChunkRead(i))
      m |= uint64_t(1) << b;
  }
  return m;
}
constexpr uint64_t kReadTouchBits = MakeReadTouchBits();
static_assert(std::popcount(kReadTouchBits) == kDeltaReadChunks.count);  // every read chunk has its bit
u32 g_marked_deltas = 0;  // under the chain's lock: the sparse check's count

inline uint64_t &DeltaMarksOf(u32 device_va) {
  return native::g_touch[native::TouchSlotOf(device_va)].chunks;
}
// A device of the chain whose marks are a sink (no slot of its own: NewOp takes a slot's).
inline bool DeltaMarksSink(u32 device_va) {
  return native::TouchSlotOf(device_va) >= native::kTouchSlots;
}
// ssasr-nx (2026-11-27, batch 5 H6): inline (a call twice a draw: 0.05-0.09 ms a frame in the console tables).
[[gnu::always_inline]] inline bool MarkedDeltasOn(u32 device_va) {
  return ::ssasr::host50::On(2) && DeltasNarrowOn() &&
         !::ssasr::host50::g_marks_failed.load(std::memory_order_relaxed) && DeltaMarksSink(device_va);
}
void TakeDeltaMarks(u32 device_va) {
  if (::ssasr::host50::On(2) && DeltaMarksSink(device_va))
    DeltaMarksOf(device_va) = 0;
}
// ssasr-nx (2026-11-27, batch 5): at a rebase (the shadow is about to be the whole device again): H32 the shadow's
// place (see DeltaChain::shadow; the device's L1 set of each line known from its host address), and the drawing
// device's slot for H2/H3 (the setters' tags kept out of it) and H31 (a new mark asks for its shadow line).
// ssasr-nx (2026-11-27, the thread batch F1): a rebuilt chunk that differed from the device at a sparse check (a setter
// write the log missed): the log is off for the rest of the run (as a missed mark turns the marks off).
std::atomic<bool> g_setter_log_failed{false};
std::atomic<uint64_t> g_log_chunks{0}, g_log_entries{0}, g_log_overflows{0};

// ssasr-nx (2026-11-27, the thread batch T1): the shadow's place alone (the submit worker's part when it owns the
// shadow: FinishStagedCapture's rebase).
void PlaceShadowBuffer(const u8 *bytes) {
  namespace h50 = ::ssasr::host50;
  DeltaChain &c = g_delta;
  if (h50::On(h50::H(32))) {
    const uintptr_t dev = reinterpret_cast<uintptr_t>(bytes), st = reinterpret_cast<uintptr_t>(c.shadow_storage);
    uintptr_t at = (st & ~uintptr_t(16383)) + ((dev + 8192) & 16383);
    if (at < st)
      at += 16384;
    if (u8 *const placed = reinterpret_cast<u8 *>(at); placed != c.shadow) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        RGPU_INFO("[renut] ssasr batch 5 H32: the delta shadow at L1 offset {:#x} of 16 KB (the device's {:#x}; it "
                  "was {:#x})",
                  at & 16383, dev & 16383, reinterpret_cast<uintptr_t>(c.shadow) & 16383);
      }
      c.shadow = placed;
    }
  }
}
// 'staged' (the thread batch T1: the submit worker owns the shadow): the setters' slots only; no setter asks for a
// shadow line (H31's prefetch: the worker's lines, on another core), and the place is the worker's to set.
void PlaceDeltaShadow(u32 device_va, const u8 *bytes, bool staged = false) {
  namespace h50 = ::ssasr::host50;
  DeltaChain &c = g_delta;
  if (!staged)
    PlaceShadowBuffer(bytes);
  const bool marked = MarkedDeltasOn(device_va);
  const u32 slot = native::TouchSlotOf(device_va);
  native::g_chain_tags_apart_slot = marked && h50::On(h50::H(3)) ? slot : ~0u;
  native::g_chain_shadow = staged ? nullptr : c.shadow;
  native::g_chain_prefetch_slot = !staged && marked && h50::On(h50::H(31)) ? slot : ~0u;
  // ssasr-nx (2026-11-27, the thread batch F1): the setter log follows the drawing device from its rebase (the shadow
  // is the whole device again: nothing before matters).
  native::g_log_slot = staged && marked && render_thread::SetterLogOn() &&
                               !g_setter_log_failed.load(std::memory_order_relaxed)
                           ? slot
                           : ~0u;
  native::g_setter_log.count = 0;
  native::g_setter_log.overflow = false;
  native::g_log_raw = 0;
}
// ssasr-nx (2026-11-26, batch 4, after the console race of 22:50): each touch bit's chunk at compile time (its device
// offset, its delta index, and which one is the short tail chunk). The first form decided fetch-or-tail and short-or-
// whole with branches at every chunk, and compared each before the next one's lines were asked for: CaptureDelta
// took 1.07 ms a frame against the old 40-chunk compare's 0.89. Now the lines of every chunk to compare are asked for
// first, then the compares run with no branch but the loop's own, each result a bit.
struct TouchBitTable {
  u16 at[64] = {};
  u8 chunk[64] = {};
  uint64_t short_bits = 0;
};
constexpr TouchBitTable MakeTouchBitTable() {
  TouchBitTable t{};
  for (u32 b = 0; b < 64; ++b) {
    const u32 i = TouchBitChunk(b);
    if (i >= kDeltaChunks)
      continue;
    t.at[b] = u16(DeltaChunkOffset(i));
    t.chunk[b] = u8(i);
    if (DeltaChunkSize(i) != kDeltaUnitBytes)
      t.short_bits |= uint64_t(1) << b;
  }
  return t;
}
constexpr TouchBitTable kTouchBit = MakeTouchBitTable();
static_assert(std::popcount(kTouchBit.short_bits & kReadTouchBits) <= 1);

// The chunks of 'bits' that differ from the shadow, as touch bits: every line asked for first, then the compares.
[[gnu::always_inline]] inline uint64_t DifferingChunks(uint64_t bits, const u8 *bytes, const u8 *shadow, bool neon) {
  for (uint64_t m = bits; m; m &= m - 1) {
    const u32 at = kTouchBit.at[std::countr_zero(m)];
    __builtin_prefetch(bytes + at);
    __builtin_prefetch(shadow + at);
  }
  uint64_t differ = 0;
  for (uint64_t m = bits & ~kTouchBit.short_bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b];
    differ |= uint64_t(banjo::renut_gpu::Line64Differs(bytes + at, shadow + at, neon)) << b;
  }
  for (uint64_t m = bits & kTouchBit.short_bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b];
    differ |= uint64_t(ShortChunkDiffers(bytes + at, shadow + at, DeltaChunkSize(kTouchBit.chunk[b]))) << b;
  }
  return differ;
}

// The dirty groups of one stage (bit 63 - group) whose bytes differ from the shadow's.
inline uint64_t ChangedGroups(uint64_t dirty, const u8 *bytes, const u8 *shadow, u32 base, bool neon) {
  // ssasr-nx (2026-11-27, loads batch M8): every line asked for first, then the compares (DifferingChunks' way): the
  // two compare loops (vertex and pixel constants) were 42 samples on their loads in loadsA, 0.8 % of the game thread,
  // ~12.5 dirty groups a delta, two lines each, each load waited for before the next group's address was worked out.
  if (dirty & (dirty - 1)) {
    for (uint64_t m = dirty; m; m &= m - 1) {
      const u32 at = base + (63 - u32(std::countr_zero(m))) * kDeltaUnitBytes;
      __builtin_prefetch(bytes + at);
      __builtin_prefetch(shadow + at);
    }
  }
  uint64_t changed = 0;
  for (uint64_t m = dirty; m; m &= m - 1) {
    const u32 bit = u32(std::countr_zero(m));
    const u32 at = base + (63 - bit) * kDeltaUnitBytes;
    changed |= uint64_t(banjo::renut_gpu::Line64Differs(bytes + at, shadow + at, neon)) << bit;
  }
  return changed;
}

[[gnu::hot]] void MarkedDelta(u32 device_va, d3d::Device *device, const u8 *bytes, DrawCapture *capture) {
  namespace h50 = ::ssasr::host50;
  // ssasr-nx (2026-11-27, batch 5 N16): the delta's buffer (grown only: the pooled capture's last delta and more) fetched
  // for writing while the chunks are compared: the render thread read it last, on another core, and the push's release
  // (stlr) waits for every store before it, these misses among them. Up to 512 bytes (a delta is ~460 on average).
  if (h50::On(h50::N(16))) {
    const u8 *const out_data = capture->state.data();
    const size_t ahead = std::min<size_t>(capture->state.capacity(), 512);
    for (size_t k = 0; k < ahead; k += 64)
      __builtin_prefetch(out_data + k, 1);
  }
  DeltaChain &c = g_delta;
  const bool neon = NeonCompareOn();
  uint64_t &marks = DeltaMarksOf(device_va);
  const uint64_t touched = marks & kReadTouchBits;
  marks = 0;
  const uint64_t tag_states = uint64_t(device->m_Pending[2]), tag_fetch = uint64_t(device->m_Pending[3]),
                 tag_misc = uint64_t(device->m_Pending[4]);
  const uint64_t tagged = native::TagChunks(tag_states, tag_fetch, tag_misc) & kReadTouchBits;
  const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
  const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
  c.vs = c.ps = 0;
  for (auto &mask : device->m_Pending)
    mask = 0;
  const bool blind = h50::On(45);
  const uint64_t blind_take = blind ? touched : 0;
  const uint64_t compare = (tagged | touched) & ~blind_take;
  uint64_t take = blind_take | DifferingChunks(compare, bytes, c.shadow, neon);
  // The check: the chunks nothing named compared too (each a change the marks missed: taken, and counted).
  const int every = h50::MarkCheckEvery();
  const bool check = h50::VerifyOn() || (every > 0 && ++g_marked_deltas >= u32(every));
  if (check) {
    g_marked_deltas = 0;
    if (h50::CensusOn())
      h50::Add(h50::kMarkChecks);
    const uint64_t missed = DifferingChunks(kReadTouchBits & ~(tagged | touched), bytes, c.shadow, neon);
    for (uint64_t m = missed; m; m &= m - 1)
      h50::MarksMissed(kTouchBit.at[std::countr_zero(m)], tagged | touched);
    take |= missed;
  }
  if (h50::CensusOn())
    h50::NoteTags(tag_states, tag_fetch, tag_misc, take & ~touched);  // N13
  const bool groups_compared = h50::On(5);
  const uint64_t vs_take = groups_compared ? ChangedGroups(vs, bytes, c.shadow, kDeltaVertexF, neon) : vs;
  const uint64_t ps_take = groups_compared ? ChangedGroups(ps, bytes, c.shadow, kDeltaPixelF, neon) : ps;
  // The delta at its size, written in place: the header, the chunks in their order, the groups (the vertex shader's
  // first, each stage's from group 0: ForEachGroupRun's order, which the render thread reads).
  u32 chunk_bytes = u32(std::popcount(take & ~kTouchBit.short_bits)) * kDeltaUnitBytes;
  for (uint64_t m = take & kTouchBit.short_bits; m; m &= m - 1)
    chunk_bytes += DeltaChunkSize(kTouchBit.chunk[std::countr_zero(m)]);
  const u32 groups = u32(std::popcount(vs_take) + std::popcount(ps_take));
  const u32 size = u32(sizeof(DeltaHeader)) + chunk_bytes + groups * kDeltaUnitBytes;
  // Grown only, never shrunk: resize() fills what it adds with zeros a byte at a time (seen in the console profile),
  // and nothing reads a delta capture's size (the header says what follows; HeldBytes counts the capacity).
  auto &out = capture->state;
  NoteGrow(kGrowMarked, out.capacity(), size);  // ssasr-nx (the malloc census)
  GrowWithRoom(out, size, 2048);
  if (out.size() < size)
    out.resize(size);
  u8 *const begin = out.data();
  u8 *to = begin + sizeof(DeltaHeader);
  DeltaHeader h{};
  for (uint64_t m = take; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b], i = kTouchBit.chunk[b];
    if (!((kTouchBit.short_bits >> b) & 1)) [[likely]] {
      std::memcpy(c.shadow + at, bytes + at, kDeltaUnitBytes);
      std::memcpy(to, bytes + at, kDeltaUnitBytes);
      to += kDeltaUnitBytes;
    } else {
      const u32 csize = DeltaChunkSize(i);
      std::memcpy(c.shadow + at, bytes + at, csize);
      std::memcpy(to, bytes + at, csize);
      to += csize;
    }
    h.chunks[i / 64] |= uint64_t(1) << (i % 64);
  }
  for (const auto [dirty, base] : {std::pair{vs_take, kDeltaVertexF}, std::pair{ps_take, kDeltaPixelF}}) {
    // In group order (bit 63 - group: the highest bit first), as ForEachGroupRun gives them.
    for (uint64_t m = dirty; m;) {
      const u32 group = u32(std::countl_zero(m));
      const u32 at = base + group * kDeltaUnitBytes;
      std::memcpy(to, bytes + at, kDeltaUnitBytes);
      if (groups_compared)
        std::memcpy(c.shadow + at, bytes + at, kDeltaUnitBytes);
      to += kDeltaUnitBytes;
      m &= ~(uint64_t(1) << (63 - group));
    }
  }
  h.vs = vs_take;
  h.ps = ps_take;
  std::memcpy(begin, &h, sizeof(h));
  capture->snapshot = SnapshotKind::Delta;
  ++c.deltas;
  c.bytes += size;
  const u32 chunks = u32(std::popcount(take));
  c.chunks += chunks;
  c.groups += groups;
  if (h50::CensusOn()) {
    h50::Add(h50::kDeltas);
    h50::Add(h50::kChunksMarked, uint64_t(std::popcount(tagged | touched)));
    h50::Add(h50::kChunksCompared, uint64_t(std::popcount(compare)));
    h50::Add(h50::kChunksTaken, chunks);
    h50::Add(h50::kChunksTakenBlind, uint64_t(std::popcount(blind_take)));
    if (!(tagged | touched | vs | ps))
      h50::Add(h50::kEmptyDeltas);
    h50::Add(h50::kGroupsDirty, uint64_t(std::popcount(vs) + std::popcount(ps)));
    h50::Add(h50::kGroupsSame, uint64_t(std::popcount(vs) + std::popcount(ps)) - groups);
    h50::Add(h50::kDeltaBytes, size);
  }
}

// ---------------------------------------------------------------------------
// ssasr-nx (2026-11-27, the thread batch T1 / T3 / T21 / T22; out/thread_plan.md): with the submit worker
// (render_thread.cpp, ssasr_submit_worker) the chain's shadow is the worker's. The game thread stages a delta: a header
// with its masks, the chunks it names raw (touch-bit order) and its dirty constant groups raw (group order), with no
// compare and no shadow; the worker's FinishStagedCapture compares them with the shadow, keeps the shadow, and
// compacts the buffer in place into the delta the render thread reads (DeltaHeader, the chunks and groups taken):
// byte for byte what MarkedDelta or the compare of every read chunk would have written, the compares made before any
// shadow update as there. A rebase stages the whole device as before; the worker places the shadow and copies it in.
// The order is the chain's: its captures are queued in the order they are made (a deferred one under the chain's
// lock before the next draw of its device), and the worker takes them in queue order.
// ---------------------------------------------------------------------------
struct StageHeader {
  uint64_t compare = 0;  // touch bits compared with the shadow (taken if they differ)
  uint64_t blind = 0;    // touch bits taken without a compare (#45)
  uint64_t check = 0;    // touch bits of the sparse check (compared; a difference is a missed mark, taken)
  uint64_t vs = 0, ps = 0;  // the dirty groups staged (bit 63 - group), the vertex stage's first
  uint64_t marked = 0;      // tagged | touched (MarksMissed's report)
  // ssasr-nx (2026-11-27, the thread batch F1): of the bits above, those the worker rebuilds from its shadow and the
  // setter log (no raw bytes staged), and of those, the ones staged raw as well for the sparse check (the rebuilt chunk
  // compared with the device's).
  uint64_t log = 0;
  uint64_t logcheck = 0;
  uint32_t groups_compared = 0;
  uint32_t log_count = 0;  // F1: the log entries staged after the raw chunks
  uint64_t pad[3] = {};
};
static_assert(sizeof(StageHeader) == 96 && sizeof(StageHeader) >= sizeof(DeltaHeader),
              "the staged chunks start at or after the delta's: the compaction never writes ahead of what it reads");
// The bytes a touch bit's chunk takes (the short tail chunk's own size).
[[gnu::always_inline]] inline u32 TouchChunkBytes(u32 b) {
  return ((kTouchBit.short_bits >> b) & 1) ? DeltaChunkSize(kTouchBit.chunk[b]) : kDeltaUnitBytes;
}
[[gnu::always_inline]] inline u32 TouchBitsBytes(uint64_t bits) {
  u32 n = u32(std::popcount(bits & ~kTouchBit.short_bits)) * kDeltaUnitBytes;
  for (uint64_t m = bits & kTouchBit.short_bits; m; m &= m - 1)
    n += DeltaChunkSize(kTouchBit.chunk[std::countr_zero(m)]);
  return n;
}
std::atomic<uint64_t> g_staged_deltas{0}, g_staged_bytes{0}, g_staged_chunks{0}, g_staged_groups{0};

// Staging is on: the submit worker and the narrow chunk list (the read chunks are the 64 touch bits). (A staged
// capture is never held as a tile twin: TakeTwinCapture.)
bool StagingOn() {
  static const bool on = render_thread::SubmitWorkerOn() && DeltasNarrowOn();
  return on;
}

// The game thread's part: the header and the raw bytes into the capture's state (grown only, as MarkedDelta's). F1
// (sh.log): the chunks rebuilt from the log are not copied; the log's entries that touch them follow the raw chunks
// (the setter log is then empty for the next draw: the caller's), and the state is grown to the finished delta's most
// so that the worker never grows it.
[[gnu::hot]] void StageChunks(DrawCapture *capture, const u8 *bytes, StageHeader &sh) {
  const uint64_t bits = sh.compare | sh.blind | sh.check;
  const uint64_t raw = (bits & ~sh.log) | sh.logcheck;
  const u32 groups_bytes = u32(std::popcount(sh.vs) + std::popcount(sh.ps)) * kDeltaUnitBytes;
  u32 log_count = 0;
  if (sh.log) {
    const native::SetterLog &l = native::g_setter_log;
    for (u32 k = 0; k < l.count; ++k) {
      const native::SetterLogEntry &e = l.entry[k];
      if ((native::TouchBit(e.offset) | native::TouchBit(e.offset + e.size - 1u)) & sh.log)
        ++log_count;
    }
  }
  sh.log_count = log_count;
  const u32 size = u32(sizeof(StageHeader)) + TouchBitsBytes(raw) + log_count * u32(sizeof(native::SetterLogEntry)) +
                   groups_bytes;
  const u32 finished_most = sh.log ? u32(sizeof(DeltaHeader)) + TouchBitsBytes(bits) + groups_bytes : 0;
  // Every line asked for first (DifferingChunks' way), then the copies.
  for (uint64_t m = raw; m; m &= m - 1)
    __builtin_prefetch(bytes + kTouchBit.at[std::countr_zero(m)]);
  auto &out = capture->state;
  NoteGrow(kGrowStaged, out.capacity(), std::max(size, finished_most));  // ssasr-nx (the malloc census)
  GrowWithRoom(out, std::max(size, finished_most), 2048);
  if (out.size() < std::max(size, finished_most))
    out.resize(std::max(size, finished_most));
  u8 *const begin = out.data();
  std::memcpy(begin, &sh, sizeof(sh));
  u8 *to = begin + sizeof(StageHeader);
  for (uint64_t m = raw; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b];
    if (!((kTouchBit.short_bits >> b) & 1)) [[likely]] {
      std::memcpy(to, bytes + at, kDeltaUnitBytes);
      to += kDeltaUnitBytes;
    } else {
      const u32 csize = DeltaChunkSize(kTouchBit.chunk[b]);
      std::memcpy(to, bytes + at, csize);
      to += csize;
    }
  }
  if (log_count) {
    const native::SetterLog &l = native::g_setter_log;
    for (u32 k = 0; k < l.count; ++k) {
      const native::SetterLogEntry &e = l.entry[k];
      if ((native::TouchBit(e.offset) | native::TouchBit(e.offset + e.size - 1u)) & sh.log) {
        std::memcpy(to, &e, sizeof(e));
        to += sizeof(e);
      }
    }
  }
  for (const auto [dirty, base] : {std::pair{sh.vs, kDeltaVertexF}, std::pair{sh.ps, kDeltaPixelF}}) {
    for (uint64_t m = dirty; m;) {
      const u32 group = u32(std::countl_zero(m));
      std::memcpy(to, bytes + base + group * kDeltaUnitBytes, kDeltaUnitBytes);
      to += kDeltaUnitBytes;
      m &= ~(uint64_t(1) << (63 - group));
    }
  }
  capture->staged = true;
  capture->snapshot = SnapshotKind::Delta;
}

// MarkedDelta's game-thread half: its masks worked out and cleared exactly as there, the bytes staged.
[[gnu::hot]] void MarkedStage(u32 device_va, d3d::Device *device, const u8 *bytes, DrawCapture *capture) {
  namespace h50 = ::ssasr::host50;
  if (h50::On(h50::N(16))) {  // MarkedDelta's N16: the buffer fetched for writing
    const u8 *const out_data = capture->state.data();
    const size_t ahead = std::min<size_t>(capture->state.capacity(), 512);
    for (size_t k = 0; k < ahead; k += 64)
      __builtin_prefetch(out_data + k, 1);
  }
  DeltaChain &c = g_delta;
  uint64_t &marks = DeltaMarksOf(device_va);
  const uint64_t touched = marks & kReadTouchBits;
  marks = 0;
  const uint64_t st = uint64_t(device->m_Pending[2]), ft = uint64_t(device->m_Pending[3]),
                 mt = uint64_t(device->m_Pending[4]);  // (N18's census reads them after the clear below)
  const uint64_t tagged = native::TagChunks(st, ft, mt) & kReadTouchBits;
  const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
  const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
  c.vs = c.ps = 0;
  for (auto &mask : device->m_Pending)
    mask = 0;
  const uint64_t blind_take = h50::On(45) ? touched : 0;
  StageHeader sh;
  sh.compare = (tagged | touched) & ~blind_take;
  sh.blind = blind_take;
  const int every = h50::MarkCheckEvery();
  bool check_draw = false;
  if (h50::VerifyOn() || (every > 0 && ++g_marked_deltas >= u32(every))) {
    g_marked_deltas = 0;
    if (h50::CensusOn())
      h50::Add(h50::kMarkChecks);
    sh.check = kReadTouchBits & ~(tagged | touched);
    check_draw = true;
  }
  sh.vs = vs;
  sh.ps = ps;
  sh.marked = tagged | touched;
  if (h50::CensusOn()) [[unlikely]] {  // ssasr-nx (2026-11-27): N18's census after T1 (port 388: the port's map)
    g_n18_draws.fetch_add(1, std::memory_order_relaxed);
    g_n18_raw_chunks.fetch_add(uint64_t(std::popcount(sh.compare | sh.blind | sh.check)), std::memory_order_relaxed);
    if (st | mt | (ft >> 32)) {
      g_n18_state_tagged.fetch_add(1, std::memory_order_relaxed);
      if (st && mt == 0 && (ft >> 32) == 0) {
        g_n18_states_alone.fetch_add(1, std::memory_order_relaxed);
        for (uint64_t bits = st; bits; bits &= bits - 1)
          g_n18_tag_draws[std::countr_zero(bits)].fetch_add(1, std::memory_order_relaxed);
        if (native::g_narrow_tags && (st & ~native::g_narrow_tags) == 0) {
          uint64_t narrow_tag = 0;
          for (uint64_t bits = st; bits; bits &= bits - 1)
            narrow_tag |= native::g_narrow_chunks[std::countr_zero(bits)];
          for (uint32_t bits = uint32_t(ft); bits; bits &= bits - 1)
            narrow_tag |= native::kFetchTagChunks.chunks[31 - std::countr_zero(bits)];
          g_n18_in_map.fetch_add(1, std::memory_order_relaxed);
          g_n18_chunks_now.fetch_add(uint64_t(std::popcount(sh.compare | sh.blind)), std::memory_order_relaxed);
          g_n18_chunks_narrow.fetch_add(uint64_t(std::popcount(((narrow_tag & kReadTouchBits) | touched))),
                                        std::memory_order_relaxed);
        }
      }
    }
  }
  sh.groups_compared = h50::On(5) ? 1 : 0;
  // ssasr-nx (2026-11-27, the thread batch F1): the chunks only the setters wrote, rebuilt on the worker from the log
  // (none when it overflowed, or this is not its device); at a sparse check they are staged raw too and compared.
  native::SetterLog &setter_log = native::g_setter_log;
  if (native::g_log_slot == native::TouchSlotOf(device_va)) {
    if (!setter_log.overflow && !g_setter_log_failed.load(std::memory_order_relaxed))
      sh.log = touched & ~tagged & ~native::g_log_raw & (sh.compare | sh.blind);
    else
      g_log_overflows.fetch_add(setter_log.overflow ? 1 : 0, std::memory_order_relaxed);
    sh.logcheck = check_draw ? sh.log : 0;
  }
  StageChunks(capture, bytes, sh);
  setter_log.count = 0;  // (F1: the next draw's writes from here)
  setter_log.overflow = false;
  native::g_log_raw = 0;
  ++c.deltas;
  if (h50::CensusOn()) {
    h50::Add(h50::kChunksMarked, uint64_t(std::popcount(tagged | touched)));
    h50::Add(h50::kChunksCompared, uint64_t(std::popcount(sh.compare)));
    h50::Add(h50::kChunksTakenBlind, uint64_t(std::popcount(blind_take)));
    if (!(tagged | touched | vs | ps))
      h50::Add(h50::kEmptyDeltas);
    h50::Add(h50::kGroupsDirty, uint64_t(std::popcount(vs) + std::popcount(ps)));
  }
}

// The compare of every read chunk's game-thread half (CaptureDelta's own path with the narrow list: the marks off or
// failed): the masks as there, every read chunk staged for a compare, the groups taken whole.
[[gnu::hot]] void CompareAllStage(d3d::Device *device, const u8 *bytes, DrawCapture *capture) {
  DeltaChain &c = g_delta;
  const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
  const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
  c.vs = c.ps = 0;
  for (auto &mask : device->m_Pending)
    mask = 0;
  StageHeader sh;
  sh.compare = kReadTouchBits;
  sh.vs = vs;
  sh.ps = ps;
  sh.groups_compared = 0;
  StageChunks(capture, bytes, sh);
  native::g_setter_log.count = 0;  // (F1: every read chunk copied raw here: the log starts again)
  native::g_setter_log.overflow = false;
  native::g_log_raw = 0;
  ++c.deltas;
}

// ssasr-nx (2026-11-27, the thread batch F1): a staged delta with chunks to rebuild from the setter log. The rebuilt
// chunks take no staged bytes, so the delta is written into the worker's own buffer and copied back (the game thread
// grew the state for it: StageChunks' finished_most). The same compares, order and shadow updates as the form below.
void FinishStagedWithLog(DrawCapture *capture, const StageHeader &sh) {
  namespace h50 = ::ssasr::host50;
  DeltaChain &c = g_delta;
  const bool neon = NeonCompareOn();
  u8 *const begin = capture->state.data();
  const uint64_t bits = sh.compare | sh.blind | sh.check;
  const uint64_t raw = (bits & ~sh.log) | sh.logcheck;
  // Where each raw chunk is, then the log's entries, then the groups.
  const u8 *raw_at[64] = {};
  const u8 *in = begin + sizeof(StageHeader);
  for (uint64_t m = raw; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    raw_at[b] = in;
    in += TouchChunkBytes(b);
  }
  const u8 *const entries = in;
  in += sh.log_count * sizeof(native::SetterLogEntry);
  const u8 *const groups_in = in;
  // The rebuilt chunks: the shadow's (the device at the last draw), then every logged write in its order.
  alignas(64) static thread_local u8 image[64][kDeltaUnitBytes];
  for (uint64_t m = sh.log; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    std::memcpy(image[b], c.shadow + kTouchBit.at[b], TouchChunkBytes(b));
  }
  for (u32 k = 0; k < sh.log_count; ++k) {
    native::SetterLogEntry e;
    std::memcpy(&e, entries + k * sizeof(native::SetterLogEntry), sizeof(e));
    const u8 *src = reinterpret_cast<const u8 *>(&e.bytes);
    for (u32 j = 0; j < e.size; ++j) {
      const u32 at = u32(e.offset) + j;
      const uint64_t bit = native::TouchBit(at);
      if (bit & sh.log) {
        const u32 b = u32(std::countr_zero(bit));
        image[b][at - kTouchBit.at[b]] = src[j];
      }
    }
  }
  g_log_chunks.fetch_add(uint64_t(std::popcount(sh.log)), std::memory_order_relaxed);
  g_log_entries.fetch_add(sh.log_count, std::memory_order_relaxed);
  // Each chunk's bytes: rebuilt, or raw; at a sparse check a rebuilt chunk that differs from the device's turns the log
  // off for the run, and the device's bytes are used.
  const u8 *chunk[64] = {};
  for (uint64_t m = bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    chunk[b] = ((sh.log >> b) & 1) ? image[b] : raw_at[b];
  }
  for (uint64_t m = sh.logcheck; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    if (std::memcmp(image[b], raw_at[b], TouchChunkBytes(b)) != 0) {
      chunk[b] = raw_at[b];
      if (!g_setter_log_failed.exchange(true, std::memory_order_relaxed)) {
        // The bytes that differ (device offset, rebuilt, device), up to eight, and how many log entries touched it.
        std::string where;
        int shown = 0;
        for (u32 j = 0; j < TouchChunkBytes(b) && shown < 8; ++j) {
          if (image[b][j] != raw_at[b][j]) {
            where += fmt::format(" +{:#x}: {:02x} not {:02x};", u32(kTouchBit.at[b]) + j, image[b][j], raw_at[b][j]);
            ++shown;
          }
        }
        u32 touching = 0;
        for (u32 k = 0; k < sh.log_count; ++k) {
          native::SetterLogEntry e;
          std::memcpy(&e, entries + k * sizeof(native::SetterLogEntry), sizeof(e));
          touching += ((native::TouchBit(e.offset) | native::TouchBit(e.offset + e.size - 1u)) >> b) & 1;
        }
        RGPU_WARN("[renut] ssasr thread batch F1: device chunk +{:#x} rebuilt from the setter log differs from the "
                  "device ({} of the draw's {} log entries touch it;{}): the setter log is off for the rest of the run",
                  u32(kTouchBit.at[b]), touching, sh.log_count, where);
      }
    }
  }
  // The compares, every one before any shadow update.
  uint64_t take = sh.blind;
  for (uint64_t m = bits & ~sh.blind; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b];
    const u32 csize = TouchChunkBytes(b);
    const bool differs = csize != kDeltaUnitBytes ? ShortChunkDiffers(chunk[b], c.shadow + at, csize)
                                                  : banjo::renut_gpu::Line64Differs(chunk[b], c.shadow + at, neon);
    if (differs) {
      take |= uint64_t(1) << b;
      if ((sh.check >> b) & 1)
        h50::MarksMissed(at, sh.marked);
    }
  }
  uint64_t taken_groups[2] = {0, 0};
  {
    const u8 *g = groups_in;
    int stage = 0;
    for (const auto [dirty, base] : {std::pair{sh.vs, kDeltaVertexF}, std::pair{sh.ps, kDeltaPixelF}}) {
      for (uint64_t m = dirty; m;) {
        const u32 group = u32(std::countl_zero(m));
        const uint64_t bit = uint64_t(1) << (63 - group);
        if (!sh.groups_compared ||
            banjo::renut_gpu::Line64Differs(g, c.shadow + base + group * kDeltaUnitBytes, neon))
          taken_groups[stage] |= bit;
        g += kDeltaUnitBytes;
        m &= ~bit;
      }
      ++stage;
    }
  }
  // The delta, in the worker's buffer, then over the staged bytes.
  alignas(64) static thread_local u8 out_buf[kDeltaMaxBytes];
  DeltaHeader h{};
  u8 *out = out_buf + sizeof(DeltaHeader);
  for (uint64_t m = bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    if (!((take >> b) & 1))
      continue;
    const u32 at = kTouchBit.at[b], i = kTouchBit.chunk[b];
    const u32 csize = TouchChunkBytes(b);
    std::memcpy(c.shadow + at, chunk[b], csize);
    std::memcpy(out, chunk[b], csize);
    out += csize;
    h.chunks[i / 64] |= uint64_t(1) << (i % 64);
  }
  {
    const u8 *g = groups_in;
    int stage = 0;
    for (const auto [dirty, base] : {std::pair{sh.vs, kDeltaVertexF}, std::pair{sh.ps, kDeltaPixelF}}) {
      for (uint64_t m = dirty; m;) {
        const u32 group = u32(std::countl_zero(m));
        const uint64_t bit = uint64_t(1) << (63 - group);
        if (taken_groups[stage] & bit) {
          if (sh.groups_compared)
            std::memcpy(c.shadow + base + group * kDeltaUnitBytes, g, kDeltaUnitBytes);
          std::memcpy(out, g, kDeltaUnitBytes);
          out += kDeltaUnitBytes;
        }
        g += kDeltaUnitBytes;
        m &= ~bit;
      }
      ++stage;
    }
  }
  h.vs = taken_groups[0];
  h.ps = taken_groups[1];
  std::memcpy(out_buf, &h, sizeof(h));
  const u32 size = u32(out - out_buf);
  std::memcpy(begin, out_buf, size);  // (state.size() is at least the finished delta's most: StageChunks)
  capture->staged = false;
  const u32 chunks = u32(std::popcount(take));
  const u32 groups = u32(std::popcount(taken_groups[0]) + std::popcount(taken_groups[1]));
  g_staged_deltas.fetch_add(1, std::memory_order_relaxed);
  g_staged_bytes.fetch_add(size, std::memory_order_relaxed);
  g_staged_chunks.fetch_add(chunks, std::memory_order_relaxed);
  g_staged_groups.fetch_add(groups, std::memory_order_relaxed);
  if (h50::CensusOn()) {
    h50::Add(h50::kDeltas);
    h50::Add(h50::kChunksTaken, chunks);
    h50::Add(h50::kGroupsSame, uint64_t(std::popcount(sh.vs) + std::popcount(sh.ps)) - groups);
    h50::Add(h50::kDeltaBytes, size);
  }
}

// The submit worker's part ('pre' of the capture's item): a rebase's shadow, or a staged delta finished.
void FinishStagedCapture(void *arg) {
  namespace h50 = ::ssasr::host50;
  DrawCapture *capture = static_cast<DrawCapture *>(arg);
  DeltaChain &c = g_delta;
  if (capture->snapshot == SnapshotKind::Rebase) {
    PlaceShadowBuffer(GuestPtr<const u8>(capture->staged_va));
    std::memcpy(c.shadow, capture->state.data(), d3d::kDeviceSnapshotSize);
    capture->staged = false;
    return;
  }
  const bool neon = NeonCompareOn();
  u8 *const begin = capture->state.data();
  StageHeader sh;
  std::memcpy(&sh, begin, sizeof(sh));
  if (sh.log) {  // ssasr-nx (the thread batch F1)
    FinishStagedWithLog(capture, sh);
    return;
  }
  const uint64_t bits = sh.compare | sh.blind | sh.check;
  for (uint64_t m = bits; m; m &= m - 1)
    __builtin_prefetch(c.shadow + kTouchBit.at[std::countr_zero(m)]);
  // The compares, every one before any shadow update (MarkedDelta's order).
  const u8 *in = begin + sizeof(StageHeader);
  uint64_t take = sh.blind;
  for (uint64_t m = bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b];
    const bool is_short = (kTouchBit.short_bits >> b) & 1;
    const u32 csize = is_short ? DeltaChunkSize(kTouchBit.chunk[b]) : kDeltaUnitBytes;
    if (!((sh.blind >> b) & 1)) {
      const bool differs = is_short ? ShortChunkDiffers(in, c.shadow + at, csize)
                                    : banjo::renut_gpu::Line64Differs(in, c.shadow + at, neon);
      if (differs) {
        take |= uint64_t(1) << b;
        if ((sh.check >> b) & 1)
          h50::MarksMissed(at, sh.marked);
      }
    }
    in += csize;
  }
  uint64_t taken_groups[2] = {0, 0};
  const u8 *const groups_in = in;
  {
    const u8 *g = groups_in;
    int stage = 0;
    for (const auto [dirty, base] : {std::pair{sh.vs, kDeltaVertexF}, std::pair{sh.ps, kDeltaPixelF}}) {
      for (uint64_t m = dirty; m;) {
        const u32 group = u32(std::countl_zero(m));
        const uint64_t bit = uint64_t(1) << (63 - group);
        if (!sh.groups_compared ||
            banjo::renut_gpu::Line64Differs(g, c.shadow + base + group * kDeltaUnitBytes, neon))
          taken_groups[stage] |= bit;
        g += kDeltaUnitBytes;
        m &= ~bit;
      }
      ++stage;
    }
  }
  // The copies: the shadow first (from the staged bytes), then the bytes down into the delta's place.
  DeltaHeader h{};
  u8 *out = begin + sizeof(DeltaHeader);
  in = begin + sizeof(StageHeader);
  for (uint64_t m = bits; m; m &= m - 1) {
    const u32 b = u32(std::countr_zero(m));
    const u32 at = kTouchBit.at[b], i = kTouchBit.chunk[b];
    const u32 csize = ((kTouchBit.short_bits >> b) & 1) ? DeltaChunkSize(i) : kDeltaUnitBytes;
    if ((take >> b) & 1) {
      std::memcpy(c.shadow + at, in, csize);
      std::memmove(out, in, csize);
      out += csize;
      h.chunks[i / 64] |= uint64_t(1) << (i % 64);
    }
    in += csize;
  }
  {
    int stage = 0;
    for (const auto [dirty, base] : {std::pair{sh.vs, kDeltaVertexF}, std::pair{sh.ps, kDeltaPixelF}}) {
      for (uint64_t m = dirty; m;) {
        const u32 group = u32(std::countl_zero(m));
        const uint64_t bit = uint64_t(1) << (63 - group);
        if (taken_groups[stage] & bit) {
          if (sh.groups_compared)
            std::memcpy(c.shadow + base + group * kDeltaUnitBytes, in, kDeltaUnitBytes);
          std::memmove(out, in, kDeltaUnitBytes);
          out += kDeltaUnitBytes;
        }
        in += kDeltaUnitBytes;
        m &= ~bit;
      }
      ++stage;
    }
  }
  h.vs = taken_groups[0];
  h.ps = taken_groups[1];
  std::memcpy(begin, &h, sizeof(h));
  capture->staged = false;
  const u32 size = u32(out - begin);
  const u32 chunks = u32(std::popcount(take));
  const u32 groups = u32(std::popcount(taken_groups[0]) + std::popcount(taken_groups[1]));
  g_staged_deltas.fetch_add(1, std::memory_order_relaxed);
  g_staged_bytes.fetch_add(size, std::memory_order_relaxed);
  g_staged_chunks.fetch_add(chunks, std::memory_order_relaxed);
  g_staged_groups.fetch_add(groups, std::memory_order_relaxed);
  if (h50::CensusOn()) {  // (the census's counters are plain adds: one lost now and then with two threads adding)
    h50::Add(h50::kDeltas);
    h50::Add(h50::kChunksTaken, chunks);
    h50::Add(h50::kGroupsSame, uint64_t(std::popcount(sh.vs) + std::popcount(sh.ps)) - groups);
    h50::Add(h50::kDeltaBytes, size);
  }
}

[[gnu::hot]] DrawCapture *CaptureDelta(u32 device_va, const DrawCall &call) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return nullptr;
  DeltaChain &c = g_delta;
  // ssasr-nx (batch 5 N23): a Begin*Vertices draw's capture (QueueDeferredDraw) from the big ones: its vertices follow.
  // ssasr-nx (2026-11-27, the malloc follow-up's fix): a rebase too (the whole device, 12,888 bytes): it took a small
  // capture and grew it (the PC census: up to three in four of the rebases), which then went back to the big ones.
  const bool rebase = c.device.load(std::memory_order_relaxed) != device;
  DrawCapture *capture =
      (g_capture_big_next || rebase) && ::ssasr::host50::On(::ssasr::host50::N(23))
          ? g_capture_pool.TakeBig(rebase ? CapturePool::BigNeed::kState : CapturePool::BigNeed::kVertices)
          : g_capture_pool.Take(true);
  capture->call = call;
  capture->call.up_vertices = nullptr;
  capture->call.up_indices = nullptr;
  const auto *bytes = reinterpret_cast<const u8 *>(device);
  CaptureStackHeaders(bytes, capture->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  const bool staging = StagingOn();  // ssasr-nx (2026-11-27, the thread batch T1): the submit worker owns the shadow
  if (rebase) {
    // The chain's first draw (or a frame's): the whole device, which the render thread takes as its own.
    for (auto &mask : device->m_Pending)
      mask = 0;
    TakeDeltaMarks(device_va);  // ssasr-nx (2026-11-26, batch 4 #2): the shadow is the whole device now
    NoteGrow(kGrowRebase, capture->state.capacity(), d3d::kDeviceSnapshotSize);  // ssasr-nx (the malloc census)
    capture->state.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
    PlaceDeltaShadow(device_va, bytes, staging);  // ssasr-nx (2026-11-27, batch 5 H3, H31, H32)
    if (staging) {  // (the thread batch T1: the worker places the shadow and copies the device in)
      capture->staged = true;
      capture->staged_va = device_va;
    } else {
      std::memcpy(c.shadow, bytes, d3d::kDeviceSnapshotSize);
    }
    c.device.store(device, std::memory_order_relaxed);
    c.vs = c.ps = 0;
    capture->snapshot = SnapshotKind::Rebase;
    ++c.rebases;
    c.bytes += d3d::kDeviceSnapshotSize;
  } else if (MarkedDeltasOn(device_va)) {
    if (staging)
      MarkedStage(device_va, device, bytes, capture);  // ssasr-nx (the thread batch T1)
    else
      MarkedDelta(device_va, device, bytes, capture);  // ssasr-nx (2026-11-26, batch 4 #2-#6, #43-#45)
  } else if (staging) {
    CompareAllStage(device, bytes, capture);  // ssasr-nx (the thread batch T1)
  } else {
    const uint64_t vs = c.vs | uint64_t(device->m_Pending[0]);
    const uint64_t ps = c.ps | uint64_t(device->m_Pending[1]);
    c.vs = c.ps = 0;
    for (auto &mask : device->m_Pending)
      mask = 0;
    std::vector<u8> &out = capture->state;
    // bangaio-nx (2026-10-29, banjo_renut_small_captures): into the chain's scratch, copied into the capture at its size
    // below; off, into the capture's own storage at its greatest size.
    const bool small = SmallCapturesOn();
    if (!small && out.size() < kDeltaMaxBytes)
      out.resize(kDeltaMaxBytes);  // once per pooled capture: written below, never cleared
    u8 *const begin = small ? c.scratch : out.data();
    u8 *to = begin + sizeof(DeltaHeader);
    DeltaHeader h{};
    u32 chunks = 0;
    // A changed chunk: into the shadow and the delta, and its bit. bangaio-nx (2026-10-29, ARM): a whole chunk with
    // constant-size copies (inline q-register pairs; a variable size was two memcpy calls).
    const auto take = [&](u32 i) {
      const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
      if (size == kDeltaUnitBytes) {
        std::memcpy(c.shadow + at, bytes + at, kDeltaUnitBytes);
        std::memcpy(to, bytes + at, kDeltaUnitBytes);
      } else if (DeltaCopiesOn()) {  // deadpool-nx
        ShortChunkCopy(c.shadow + at, bytes + at, size);
        ShortChunkCopy(to, bytes + at, size);
      } else {
        std::memcpy(c.shadow + at, bytes + at, size);
        std::memcpy(to, bytes + at, size);
      }
      to += size;
      h.chunks[i / 64] |= uint64_t(1) << (i % 64);
      ++chunks;
    };
    const bool narrow = DeltasNarrowOn();
    if (DeltaMasksOn()) {
      // bangaio-nx (2026-10-29, banjo_renut_delta_masks): which chunks of each run differ, then those in order.
      const DeltaRuns &runs = narrow ? kDeltaReadRuns : kDeltaAllRuns;
      for (u32 r = 0; r < runs.count; ++r) {
        const DeltaRun &run = runs.run[r];
        const u32 at = DeltaChunkOffset(run.first);
        uint32_t mask = run.short_chunk ? uint32_t(ShortChunkDiffers(bytes + at, c.shadow + at, DeltaChunkSize(run.first)))
                                        : banjo::renut_gpu::Lines64DifferMask(bytes + at, c.shadow + at, run.count);
        for (; mask; mask &= mask - 1)
          take(run.first + u32(std::countr_zero(mask)));
      }
    } else {
      const DeltaChunkList &list = narrow ? kDeltaReadChunks : kDeltaAllChunks;
      const bool neon = NeonCompareOn();  // banjo-nx (from sa2-nx) (banjo_renut_neon_compare)
      for (u32 k = 0; k < list.count; ++k) {
        const u32 i = list.index[k];
        const u32 at = DeltaChunkOffset(i), size = DeltaChunkSize(i);
        const bool differs = size == kDeltaUnitBytes ? banjo::renut_gpu::Line64Differs(bytes + at, c.shadow + at, neon)
                                                     : std::memcmp(bytes + at, c.shadow + at, size) != 0;
        if (differs)
          take(i);
      }
    }
    for (const auto [dirty, base] : {std::pair{vs, kDeltaVertexF}, std::pair{ps, kDeltaPixelF}}) {
      ForEachGroupRun(dirty, [&](u32 first, u32 count) {
        if (DeltaCopiesOn()) {  // deadpool-nx: a fixed-size copy a group
          const u8 *src = bytes + base + first * kDeltaUnitBytes;
          for (u32 k = 0; k < count; ++k, to += kDeltaUnitBytes, src += kDeltaUnitBytes)
            std::memcpy(to, src, kDeltaUnitBytes);
          return;
        }
        const u32 group_bytes = count * kDeltaUnitBytes;
        std::memcpy(to, bytes + base + first * kDeltaUnitBytes, group_bytes);
        to += group_bytes;
      });
    }
    h.vs = vs;
    h.ps = ps;
    std::memcpy(begin, &h, sizeof(h));
    if (small) {
      NoteGrow(kGrowDelta, out.capacity(), size_t(to - begin));  // ssasr-nx (the malloc census)
      out.assign(begin, to);  // the capture's storage kept (a pooled vector keeps its capacity)
    }
    capture->snapshot = SnapshotKind::Delta;
    ++c.deltas;
    c.bytes += uint64_t(to - begin);
    c.chunks += chunks;
    c.groups += uint64_t(std::popcount(vs) + std::popcount(ps));
  }
  if (DeltasVerifyOn())
    capture->verify.assign(bytes, bytes + d3d::kDeviceSnapshotSize);
  // (off: never assigned this run, nothing to clear: M7)
  return capture;
}

} // namespace

// banjo-nx (2026-11-23, banjo_renut_tile_twin_merge): the game thread's side, for render/draw.cpp's 10 s twin line.
std::string TwinQueueCensus() {
  return fmt::format("; held {}: queued with its twin {}, alone before another draw {} ({} tessellated), alone before "
                     "other work {}",
                     g_twin_holds.load(std::memory_order_relaxed), g_twin_pairs_queued.load(std::memory_order_relaxed),
                     g_twin_flush_draw.load(std::memory_order_relaxed),
                     g_twin_flush_draw_tess.load(std::memory_order_relaxed),
                     g_twin_flush_hook.load(std::memory_order_relaxed));
}

void SnapshotDeltasNewFrame() {
  if (!DeltasOn())
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device.store(nullptr, std::memory_order_relaxed);  // the frame's first draw rebases: whatever went wrong lasts a frame at most
}

void ForgetSnapshotDeltas(const void *device) {
  if (!DeltasOn())
    return;
  // banjo-nx (2026-10-26): only the chain's own device's masks matter to it; a recording device's ops (every recorded
  // op came here) took the chain's lock each, and made its next draw a whole copy, for nothing. The chain's device is
  // the title device, which the game does not record on while it draws on it elsewhere: an op of it is ordered with
  // its draws by the game, and so is this load with the store that made it the chain's.
  if (g_delta.device.load(std::memory_order_relaxed) != device)
    return;
  std::lock_guard lock(g_delta.mutex);
  g_delta.device.store(nullptr, std::memory_order_relaxed);
}

std::string SnapshotDeltaReport() {
  if (!DeltasOn())
    return {};
  uint64_t deltas, rebases, bytes, chunks, groups, whole, narrowed, narrowed_bytes, narrowed_prefix, cross;
  {
    std::lock_guard lock(g_delta.mutex);
    whole = g_delta.whole;
    g_delta.whole = 0;
    cross = g_delta.cross;  // blur-nx (2026-11-13, #51)
    g_delta.cross = 0;
    narrowed = g_delta.narrowed;  // blur-nx (2026-11-13, #13)
    narrowed_bytes = g_delta.narrowed_bytes;
    narrowed_prefix = g_delta.narrowed_prefix_bytes;
    g_delta.narrowed = g_delta.narrowed_bytes = g_delta.narrowed_prefix_bytes = 0;
    deltas = g_delta.deltas;
    rebases = g_delta.rebases;
    bytes = g_delta.bytes;
    chunks = g_delta.chunks;
    groups = g_delta.groups;
    g_delta.deltas = g_delta.rebases = g_delta.bytes = g_delta.chunks = g_delta.groups = 0;
  }
  // ssasr-nx (2026-11-27, the thread batch T22): the staged deltas' sizes, counted by the submit worker.
  bytes += g_staged_bytes.exchange(0, std::memory_order_relaxed);
  chunks += g_staged_chunks.exchange(0, std::memory_order_relaxed);
  groups += g_staged_groups.exchange(0, std::memory_order_relaxed);
  const uint64_t staged_done = g_staged_deltas.exchange(0, std::memory_order_relaxed);
  const uint64_t draws = deltas + rebases;
  if (!draws)
    return {};
  const double per_delta = double(std::max<uint64_t>(deltas, 1));
  std::string line = fmt::format(
      "device snapshots (banjo_renut_snapshot_deltas): {} draws, {:.0f} bytes kept a draw on average (the whole device "
      "is {}); {:.1f} chunks and {:.1f} constant groups a delta; {} whole copies (the frames' first draws); {} "
      "Begin*Vertices draws as whole copies ({} of them narrowed to their shaders' float constants, {:.0f} bytes a "
      "copy, {:.0f} with the prefixes alone; banjo_renut_deferred_narrow; {} checked on the render thread, {} drawn "
      "with other shaders since the start); their vertex data {} draws, {:.0f} KB (#52's census)",
      draws, double(bytes) / double(draws), d3d::kDeviceSnapshotSize, double(chunks) / per_delta,
      double(groups) / per_delta, rebases, whole, narrowed,
      narrowed ? double(narrowed_bytes) / double(narrowed) : 0.0,
      narrowed ? double(narrowed_prefix) / double(narrowed) : 0.0,
      g_narrow_checked.exchange(0, std::memory_order_relaxed),  // blur-nx (2026-11-13, #13's fix)
      g_narrow_differ.load(std::memory_order_relaxed),
      g_deferred_vertex_draws.exchange(0, std::memory_order_relaxed),
      double(g_deferred_vertex_bytes.exchange(0, std::memory_order_relaxed)) / 1024.0);
  if (DeferredChainOn())  // blur-nx (2026-11-13, #51)
    line += fmt::format("; Begin*Vertices deltas in the chain (banjo_renut_deferred_chain), {} draws of another device "
                        "made whole while one waited", cross);
  line += DrawerCensus();  // blur-nx (2026-11-13, M-31)
  if (StagingOn())  // ssasr-nx (2026-11-27, the thread batch T1)
    line += fmt::format("; {} deltas staged and finished on the submit worker", staged_done);
  if (render_thread::SetterLogOn())  // ssasr-nx (2026-11-27, the thread batch F1)
    line += fmt::format("; setter log: {} chunks rebuilt from {} entries, {} draws overflowed{}",
                        g_log_chunks.exchange(0, std::memory_order_relaxed),
                        g_log_entries.exchange(0, std::memory_order_relaxed),
                        g_log_overflows.exchange(0, std::memory_order_relaxed),
                        g_setter_log_failed.load(std::memory_order_relaxed) ? " (OFF: a rebuilt chunk differed)" : "");
  line += render_thread::SubmitWorkerReport();
  if (::ssasr::host50::CensusOn()) {  // ssasr-nx (2026-11-27, the malloc census, N18's)
    line += GrowReport();
    line += N18Report();
  }
  if (DeltasVerifyOn()) {
    line += fmt::format("; verify: {} draws checked, {} differ", g_delta_verified.exchange(0, std::memory_order_relaxed),
                        g_delta_differ.exchange(0, std::memory_order_relaxed));
  }
  return line;
}

// bangaio-nx (2026-10-29, v6, banjo_renut_delta_prefetch): the lines CaptureDelta is about to compare, the device's and
// the shadow's, asked for at once (prfm: a hint, which cannot fault). The game's own work between two draws pushes them
// out of the L1, and the compare then waited on them a group at a time.
[[gnu::hot]] void PrefetchDeltaLines(u32 device_va) {
  const u8 *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  // ssasr-nx (2026-11-26, batch 4 #4): with the marked deltas (#2) MarkedDelta asks for the lines it compares itself,
  // all of them before its first compare (the console race of 22:50: this pass, which worked the marks and the tags out
  // a second time, took 0.28 ms a frame against the old 40 chunks' 0.20).
  if (MarkedDeltasOn(device_va))
    return;
  const DeltaRuns &runs = DeltasNarrowOn() ? kDeltaReadRuns : kDeltaAllRuns;
  if (DeltaCopiesOn()) {
    // deadpool-nx (deadpool_delta_copies): nothing when the chain's device is another (CaptureDelta rebases: no compare);
    // one line in four of the guest's runs (the hardware's stream prefetch takes the lines between); the shadow stays in
    // the game thread's cache from its last compare.
    if (g_delta.device.load(std::memory_order_relaxed) != reinterpret_cast<const d3d::Device *>(device))
      return;
    for (u32 r = 0; r < runs.count; ++r) {
      const u32 at = DeltaChunkOffset(runs.run[r].first);
      for (u32 k = 0; k < runs.run[r].count; k += 4)
        __builtin_prefetch(device + at + k * kDeltaUnitBytes);
    }
    return;
  }
  const bool shadow = !StagingOn();  // ssasr-nx (the thread batch T1): the shadow is the submit worker's
  for (u32 r = 0; r < runs.count; ++r) {
    const u32 at = DeltaChunkOffset(runs.run[r].first);
    for (u32 k = 0; k < runs.run[r].count; ++k) {
      __builtin_prefetch(device + at + k * kDeltaUnitBytes);
      if (shadow)
        __builtin_prefetch(g_delta.shadow + at + k * kDeltaUnitBytes);
    }
  }
}

[[gnu::hot]] void QueueDraw(u32 device_va, const DrawCall &call, u32 up_vertex_bytes, u32 up_index_bytes) {
  // blur-nx (2026-11-18, the census's #29): the immediate draws and their queueing time on the game threads.
  struct HbScope {
    bool on;
    uint64_t t0;
    ~HbScope() {
      if (on) {
        ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kImmediateDraws);
        ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kImmediateTicks, ::renut::gpu::host_batch::Ticks() - t0);
      }
    }
  } hb_scope{::renut::gpu::host_batch::CensusOn(),
             ::renut::gpu::host_batch::CensusOn() ? ::renut::gpu::host_batch::Ticks() : 0};
  NoteDrawer(device_va);  // blur-nx (2026-11-13, M-31's census)
  namespace h50 = ::ssasr::host50;
  struct H50Scope {  // ssasr-nx (2026-11-26, batch 4 #50): the draw's host time (the census)
    uint64_t t0;
    ~H50Scope() {
      if (t0) {
        h50::Add(h50::kDrawCalls);
        h50::Add(h50::kDrawTicks, h50::Ticks() - t0);
      }
    }
  } h50_scope{h50::CensusOn() ? h50::Ticks() : 0};
  const bool deltas = DeltasOn();  // bangaio-nx (2026-10-29, ARM): its static's guard read once, not three times
  if (deltas && DeltaPrefetchOn())
    PrefetchDeltaLines(device_va);  // bangaio-nx (2026-10-29, v6)
  // ssasr-nx (2026-11-26, batch 4 #15): the two flushes' fast paths inline (their counts of this device: blur-nx's
  // blur_renut_flush_fast_local form), not two calls and their statics' guards at every draw.
  if (!(h50::On(15) && HookFastPathsOn() && RecordedVerticesNoneFor(device_va) &&
        DeferredOf(device_va).load(std::memory_order_relaxed) == 0))  // (see FlushDeferredVertices)
    FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_snapshot_deltas): made and queued under the chain's lock.
  std::unique_lock<ChainMutex> chain;
  if (deltas)
    chain = std::unique_lock<ChainMutex>(g_delta.mutex);
  // blur-nx (2026-11-13, #51): another device's Begin*Vertices delta waiting: this draw whole, outside the chain.
  const bool cross = deltas && g_delta.deferred_device && g_delta.deferred_device != device_va;
  if (cross)
    ++g_delta.cross;
  const uint64_t capture_t0 = h50::CensusOn() ? h50::Ticks() : 0;
  // ssasr-nx (2026-11-27, batch 5 N23): a DrawPrimitiveUP draw's capture from the big ones (its vertices follow; the
  // trail writer's 5-8 KB were two of the console census's allocation sites). Under the chain's lock, as CaptureDelta.
  if (deltas)
    g_capture_big_next = !cross && call.up_vertices && up_vertex_bytes + up_index_bytes >= 2048;
  DrawCapture *capture = deltas && !cross ? CaptureDelta(device_va, call) : Capture(device_va, call);
  if (deltas)
    g_capture_big_next = false;
  if (capture_t0)
    h50::Add(h50::kCaptureTicks, h50::Ticks() - capture_t0);
  if (!capture)
    return;
  if (call.up_vertices) {
    NoteGrow(kGrowUpVertices, capture->vertices.capacity(), up_vertex_bytes);  // ssasr-nx (the malloc census)
    GrowWithRoom(capture->vertices, up_vertex_bytes, 1024);
    capture->vertices.assign(call.up_vertices, call.up_vertices + up_vertex_bytes);
  }
  if (call.up_indices) {
    NoteGrow(kGrowUpIndices, capture->indices.capacity(), up_index_bytes);
    GrowWithRoom(capture->indices, up_index_bytes, 1024);
    capture->indices.assign(call.up_indices, call.up_indices + up_index_bytes);
  }
  if (const auto *guest_device = GuestPtr<const u8>(device_va)) {
    u32 vs_word;
    std::memcpy(&vs_word, guest_device + d3d::kDeviceOffsetVertexShader, 4);
    // ssasr-nx (2026-11-27, batch 5 H9): the answer kept for the vertex shader of the draw before while no binding
    // changed since (the generation read before the lookup: a change during it shows at the next draw). Under the
    // chain's lock (the draw hook's), as is the cache.
    static u32 cached_vs = 0, cached_id = 0, cached_gen = ~0u;
    const u32 vs = __builtin_bswap32(vs_word);
    if (::ssasr::host50::On(::ssasr::host50::H(9))) {
      const u32 gen = g_last_binding_gen.load(std::memory_order_acquire);
      if (vs != cached_vs || gen != cached_gen) {
        cached_id = LastBindingOf(vs);
        cached_vs = vs;
        cached_gen = gen;
      }
      capture->binding = cached_id;
    } else {
      capture->binding = LastBindingOf(vs);
    }
  }
  // banjo-nx (2026-11-23, banjo_renut_tile_twin_merge): an upper tile's tessellated draw held for its twin, or queued
  // with the one held (a draw of the chain only: under its lock, in its order).
  if (deltas && !cross && TakeTwinCapture(device_va, call, capture))
    return;
  if (::ssasr::host50::On(::ssasr::host50::N(15)))
    QueueWholeOrDelta(capture);
  else
    QueueCapture(capture);
}

void QueueDeferredDraw(u32 device_va, const DrawCall &call, const u8 *scratch, u32 bytes,
                       const u8 *index_scratch, u32 index_bytes) {
  NoteDrawer(device_va);  // blur-nx (2026-11-13, M-31's census)
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  // banjo-nx (2026-10-20, from futari-nx, banjo_renut_deferred_chain): a Begin*Vertices draw of the chain too. Made under
  // the chain's lock and queued by FlushDeferredVertices under it as well, before any other draw of the device is made
  // (every hook flushes its device's deferred draw first), so the render thread still applies the chain in order. A
  // recorded op made meanwhile (NewOp: ForgetSnapshotDeltas) only makes the next capture a whole one, queued after this
  // draw. A draw that will not be queued (no scratch) stays a whole capture outside the chain.
  // blur-nx (2026-11-13, #51): not while another device's delta waits (then a whole capture below, outside the chain).
  if (DeltasOn() && DeferredChainOn() && scratch) {
    std::unique_lock<ChainMutex> chain(g_delta.mutex);
    if (g_delta.deferred_device && g_delta.deferred_device != device_va) {
      ++g_delta.cross;  // blur-nx (2026-11-13, #51): the whole capture below
    } else {
      {
        std::lock_guard lock(g_deferred_mutex);
        auto it = g_deferred.find(device_va);
        if (it != g_deferred.end()) {
          // One not flushed (FlushDeferredVertices above queued it, so none is expected): dropped as the original
          // replaced it; its delta never reaches the render thread, so the chain rebases.
          it->second.capture->staged = false;  // ssasr-nx (the thread batch T1): never finished, never queued
          g_capture_pool.Give(it->second.capture);
          g_deferred.erase(it);
          DeferredOf(device_va).fetch_sub(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
          g_deferred_count.store(g_deferred.size(), std::memory_order_release);
          g_delta.device.store(nullptr, std::memory_order_relaxed);
        }
      }
      g_capture_big_next = true;  // ssasr-nx (batch 5 N23): under the chain's lock, read by CaptureDelta
      DrawCapture *capture = CaptureDelta(device_va, call);
      g_capture_big_next = false;
      if (!capture)
        return;
      std::lock_guard lock(g_deferred_mutex);
      if (!g_deferred.count(device_va))
        DeferredOf(device_va).fetch_add(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
      g_deferred[device_va] = {capture, scratch, bytes, index_scratch, index_bytes};
      g_deferred_count.store(g_deferred.size(), std::memory_order_release);
      g_delta.deferred_device = device_va;  // blur-nx (2026-11-13, #51): cleared when FlushDeferredVertices queues it
      return;
    }
  }
  DrawCapture *capture = nullptr;
  {
    // banjo-nx (from sa2-nx) (banjo_renut_snapshot_deltas): a whole capture (queued later than made: outside the chain), whose dirty
    // constant groups ClearPending hands to the chain's next delta.
    std::unique_lock<ChainMutex> chain;
    if (DeltasOn())
      chain = std::unique_lock<ChainMutex>(g_delta.mutex);
    capture = CaptureDeferred(device_va, call);  // blur-nx (2026-11-13, #13): Capture() unless narrowed
    if (DeltasOn())
      ++g_delta.whole;
  }
  if (!capture)
    return;
  if (!scratch) {
    g_capture_pool.Give(capture);
    return;
  }
  std::lock_guard lock(g_deferred_mutex);
  if (!g_deferred.count(device_va))
    DeferredOf(device_va).fetch_add(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
  DeferredDraw &slot = g_deferred[device_va];
  g_capture_pool.Give(slot.capture);  // one not flushed (the original replaced it the same way)
  slot = {capture, scratch, bytes, index_scratch, index_bytes};
  g_deferred_count.store(g_deferred.size(), std::memory_order_release);
}

void FlushDeferredVertices(u32 device_va) {
  FlushRecordedVertices(device_va);
  // banjo-nx (2026-10-20, from sa2-nx): none waiting, no lock. blur-nx (2026-11-17, blur_renut_flush_fast_local): none of
  // this device's (the find below looks for nothing else).
  static const bool local = REXCVAR_GET(blur_renut_flush_fast_local);
  // ssasr-nx (2026-11-27, the host follow-up): relaxed loads (were acquire: an LDAR at every hook, ~20 % of QueueDraw's
  // console samples on its three): a count read as 0 skips, one read otherwise takes the locks below, which order the
  // map; acquire never made the count fresher.
  if (HookFastPathsOn() && (local ? DeferredOf(device_va).load(std::memory_order_relaxed) == 0
                                  : g_deferred_count.load(std::memory_order_relaxed) == 0))
    return;
  // banjo-nx (2026-10-20, from futari-nx, banjo_renut_deferred_chain): a chain draw is queued under the chain's lock
  // (taken before the deferred map's, as QueueDeferredDraw takes them), so no draw of the chain is made or queued between.
  std::unique_lock<ChainMutex> chain;
  if (DeltasOn() && DeferredChainOn())
    chain = std::unique_lock<ChainMutex>(g_delta.mutex);
  DeferredDraw deferred;
  {
    std::lock_guard lock(g_deferred_mutex);
    auto it = g_deferred.find(device_va);
    if (it == g_deferred.end())
      return;
    deferred = std::move(it->second);
    g_deferred.erase(it);
    DeferredOf(device_va).fetch_sub(1, std::memory_order_acq_rel);  // blur-nx (2026-11-17)
    g_deferred_count.store(g_deferred.size(), std::memory_order_release);
  }
  NoteGrow(kGrowDeferred, deferred.capture->vertices.capacity(), deferred.bytes);  // ssasr-nx (the malloc census)
  GrowWithRoom(deferred.capture->vertices, deferred.bytes, 8192);
  deferred.capture->vertices.assign(deferred.scratch, deferred.scratch + deferred.bytes);
  if (deferred.index_scratch && deferred.index_bytes)
    deferred.capture->indices.assign(deferred.index_scratch, deferred.index_scratch + deferred.index_bytes);
  // blur-nx (2026-11-13, #52's census): the vertex bytes the flush copies (a scratch ring could leave them in place).
  // ssasr-nx (2026-11-27, batch 5 H38): a census: a load and a store (the A57 has no LSE: a fetch_add is an exclusive
  // pair); a lost count between two threads is a census's error.
  g_deferred_vertex_bytes.store(g_deferred_vertex_bytes.load(std::memory_order_relaxed) + deferred.bytes +
                                    deferred.index_bytes,
                                std::memory_order_relaxed);
  g_deferred_vertex_draws.store(g_deferred_vertex_draws.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  QueueCapture(deferred.capture);
  if (chain.owns_lock() && g_delta.deferred_device == device_va)
    g_delta.deferred_device = 0;  // blur-nx (2026-11-13, #51): its delta queued; the chain is free for any device
}

void QueueClear(u32 device_va, u32 flags, const ClearRect *rect, const float color[4], float depth,
                u32 stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  // blur-nx (2026-11-13, #55): the target words and the viewport the clear reads, not the whole device.
  ReadResolveTargets(device, args->resolve_targets);
  ReadClearViewport(device, args->clear_viewport);
  args->has_rect = rect != nullptr;
  args->rect = rect ? *rect : ClearRect{};
  std::memcpy(args->color, color, sizeof(args->color));
  args->flags = flags;
  args->depth = depth;
  args->stencil = stencil;
  CaptureStackHeaders(device, args->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  render_thread::Queue([args] {
    SetReplayStackHeaders(args->stack_headers.count ? &args->stack_headers : nullptr);
    ClearTargetsLocked(args->resolve_targets, args->clear_viewport, args->flags,
                       args->has_rect ? &args->rect : nullptr, args->color, args->depth, args->stencil);
    SetReplayStackHeaders(nullptr);
    g_capture_pool.Give(args, true);
  });
}

void QueueResolve(u32 device_va, u32 flags, const ClearRect *source_rect, u32 dest_texture_va,
                  i32 dest_x, i32 dest_y, u32 dest_level, u32 dest_slice,
                  const float clear_color[4], float clear_depth, u32 clear_stencil) {
  FlushDeferredVertices(device_va);
  RunReadyCallbacks();
  const auto *device = GuestPtr<const u8>(device_va);
  if (!device)
    return;
  // The destination header as it is now: the game often builds it on the
  // stack (as for recorded resolves).
  DrawCapture *args = g_capture_pool.Take();  // banjo-nx (2026-10-19): pooled, see DrawCapture
  // blur-nx (2026-11-13, M-35): the eight target words the resolve reads, not the whole device (12.9 KB a resolve on the
  // Game Master); args->state is left as the pool had it (not read).
  ReadResolveTargets(device, args->resolve_targets);
  args->has_rect = source_rect != nullptr;
  args->rect = source_rect ? *source_rect : ClearRect{};
  args->has_color = clear_color != nullptr;
  if (clear_color)
    std::memcpy(args->color, clear_color, sizeof(args->color));
  const auto *header = GuestPtr<const d3d::BaseTexture>(dest_texture_va);
  args->has_dest_format = header != nullptr;
  if (header)
    args->dest_format = header->Format;
  args->flags = flags;
  args->dest_texture_va = dest_texture_va;
  args->dest_x = dest_x;
  args->dest_y = dest_y;
  args->dest_level = dest_level;
  args->dest_slice = dest_slice;
  args->depth = clear_depth;
  args->stencil = clear_stencil;
  CaptureStackHeaders(device, args->stack_headers);  // blur-nx (2026-11-10, port 76; its key 215)
  render_thread::Queue([args] {
    SetReplayStackHeaders(args->stack_headers.count ? &args->stack_headers : nullptr);
    ResolveTargetsLocked(args->resolve_targets, args->flags, args->has_rect ? &args->rect : nullptr,
                         args->dest_texture_va, args->has_dest_format ? &args->dest_format : nullptr,
                         args->dest_x, args->dest_y, args->dest_level, args->dest_slice,
                         args->has_color ? args->color : nullptr, args->depth, args->stencil);
    SetReplayStackHeaders(nullptr);
    g_capture_pool.Give(args, true);
  });
}

} // namespace renut::gpu
