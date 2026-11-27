/**
 * @file    gpu/ssasr_host50.h
 * @brief   ssasr-nx (2026-11-26, batch 4, the user: "Do all 50 in current batch please"): the game thread's host code,
 *          the two tables of 25 ideas (docs/ssasr-notes.md, "The game thread's host code"). Each runtime change is exact
 *          (the same pixels) and has its own switch: ssasr_renut_host50 (all of them, on) less the items listed in
 *          ssasr_renut_host50_off ("2,5,13"). ssasr_renut_host50_verify (off) runs the checked forms (#2 compares every
 *          chunk as well and counts the changes its marks missed); ssasr_renut_host50_census (off) one 10 s line of
 *          counts (and the setters' host time, #50).
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace ssasr::host50 {

// The ideas: batch 4's 1-50; batch 5's host ideas H2-H90 as 100 + H, its codegen and game ideas C1-C100 as 200 + C,
// the ideas found on the way N1-N50 as 300 + N (out/batch5_plan.md). Every number is on by default unless
// ssasr_renut_host50_off lists it.
constexpr int kItems = 351;
constexpr int H(int n) { return 100 + n; }
constexpr int C(int n) { return 200 + n; }
constexpr int N(int n) { return 300 + n; }
struct Switches {
  bool on[kItems] = {};
  bool verify = false;
  bool census = false;
  int mark_check = 256;  // ssasr_renut_host50_mark_check (latched: a call a draw before, 0.05 ms a frame)
};
extern Switches g_switches;
extern std::atomic<bool> g_ready;
[[gnu::cold, gnu::noinline]] void LatchSwitches();
[[gnu::always_inline]] inline const Switches &S() {
  if (!g_ready.load(std::memory_order_relaxed)) [[unlikely]]
    LatchSwitches();
  return g_switches;
}
[[gnu::always_inline]] inline bool On(int item) { return S().on[item]; }
[[gnu::always_inline]] inline bool VerifyOn() { return S().verify; }
#if defined(RENUT_DIAG_CENSUS_OFF)
constexpr bool CensusOn() { return false; }
#else
[[gnu::always_inline]] inline bool CensusOn() { return S().census; }
#endif

// The census (ssasr_renut_host50_census): a load and a store a count (one game thread draws: lost increments between
// two threads are a census's error, not a fault).
enum Count : uint32_t {
  kDeltas,            // #2: deltas made
  kChunksMarked,      // #2: chunks the setters' marks and the dirty tags named (the read ones)
  kChunksCompared,    // #2: chunks compared
  kChunksTaken,       // #2: chunks that went into the delta
  kChunksTakenBlind,  // #45: of them, taken without a compare (a setter's mark: it wrote a new value)
  kMarkMisses,        // #2 verify: chunks that changed with no mark (each a bug of the marks)
  kMarkChecks,        // #2: deltas checked against the whole compare (verify, or the sparse check)
  kEmptyDeltas,       // #3: deltas with nothing marked and no constant group
  kGroupsDirty,       // #5: float constant groups the dirty masks named
  kGroupsSame,        // #5: of them, the same as the chain's shadow (left out of the delta)
  kDeltaBytes,        // #6: the deltas' bytes
  kSetterCalls,       // #50: native setter calls timed
  kSetterTicks,       // #50: their time
  kTextureCalls,      // #50: SetTexture calls
  kTextureTicks,      // #50
  kTextureSame,       // #8: SetTexture calls that changed nothing (the slot already held it)
  kSamplerCalls,      // #50: sampler state calls
  kSamplerTicks,      // #50
  kSamplerSame,       // #10: sampler state calls that changed nothing
  kStateCalls,        // #50: render state calls
  kStateTicks,        // #50
  kStreamCalls,       // #50: SetStreamSource / SetIndices calls
  kStreamTicks,       // #50
  kStreamSame,        // #12
  kDrawCalls,         // #50: QueueDraw calls
  kDrawTicks,         // #50: their time
  kCaptureTicks,      // #50: CaptureDelta's time
  kPushes,            // #1 / #30: render-thread pushes
  kPlainPushes,       // #1: of them, plain function-pointer items
  kStackChecks,       // ssasr-nx (2026-11-27, the release census): draws CaptureStackHeaders checked (Blur's fix)
  kStackSurfaces,     // ... of them, a bound render target or depth surface on the stack (the case it is for)
  kCountCount
};
extern std::atomic<uint64_t> g_count[kCountCount];
[[gnu::always_inline]] inline void Add(Count c, uint64_t v = 1) {
  g_count[c].store(g_count[c].load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
uint64_t Ticks();
// #2's sparse check (the console's guard): a change the marks missed turns the marked compare off for the run.
void MarksMissed(uint32_t chunk_offset, uint64_t marks);
[[gnu::always_inline]] inline int MarkCheckEvery() { return S().mark_check; }  // ssasr_renut_host50_mark_check
extern std::atomic<bool> g_marks_failed;
// N13 (batch 5): the dirty tags pending at a marked delta (m_Pending words 2-4: with H3, the game's own inline writes'
// and Begin/End/ResetAllState's), each bit's count and the chunks taken that no setter marked while it was pending
// (what that tag stood for, as seen). TagChunks makes any state tag compare the 17 render state chunks.
void NoteTags(uint64_t states, uint64_t fetch, uint64_t misc, uint64_t unmarked_taken);
// The 10 s line, empty with the census off; resets the counts.
std::string Report(double seconds);

}  // namespace ssasr::host50
