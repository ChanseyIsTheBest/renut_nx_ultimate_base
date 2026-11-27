// ssasr-nx (2026-11-26, batch 4): the game thread's host code, the 50 ideas' switches and census (ssasr_host50.h).
#include "renut_engine/gpu/ssasr_host50.h"

#include <bit>
#include <chrono>
#include <cstdlib>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(ssasr_renut_host50, true, "Banjo/Enhancements",
                    "ssasr-nx (2026-11-26, batch 4): the game thread's host code, the 50 ideas of the two tables "
                    "(docs/ssasr-notes.md, \"The game thread's host code\"): each exact (the same pixels). Off: every "
                    "one as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// #45 (the marked chunks taken without a compare) is off by default: the PC race (out/pc_runs/h50_verify) took 6.4-9.5
// chunks a delta that way against 3.9 the compare takes: the game sets a state and sets it back before the next draw,
// so a mark does not mean the chunk ended different, and an unchanged chunk taken costs two copies here and the
// render thread's work after.
// ssasr-nx (2026-11-27, batch 5): N20 (300 + 20, the push's publish as dmb ishst and a plain store) off too: the
// console's 5F race held the first load after the barrier as long as after the stlr (36 samples against 37): the A57
// waits for the stores before either. And N23 (323, the capture pool's big captures apart): _malloc_r the same (0.24-0.25
// ms a frame against 0.19) and three races of it 31.7-32.6 ms a frame at 1,200 draws against 5E/5F's 30.5-31.1 (its big
// takes lock the pool's mutex one at a time, against the render thread's returns).
REXCVAR_DEFINE_STRING(ssasr_renut_host50_off, "45,320,323", "Banjo/Enhancements",
                      "ssasr_renut_host50: the ideas left as before (their numbers, comma separated: \"2,5\"; by default "
                      "45, measured worse, 320 (N20), measured no better, and 323 (N23), no better and maybe worse)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ssasr_renut_host50_verify, false, "Banjo/Tests",
                    "ssasr_renut_host50's checked forms: #2's marked compare also compares every chunk and counts the "
                    "changes its marks missed (the census line), #5's left-out groups checked against the device")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(ssasr_renut_host50_census, false, "Banjo/Diagnostics",
                    "ssasr_renut_host50's census: one 10 s line (the marks, the constant groups, the setters' time "
                    "and calls that changed nothing, the pushes)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ssasr-nx (2026-11-27, batch 5 H5): 256 (was 64): no miss in two console races and the PC verify after the
// ClearPending fix; the check compares all 40 chunks.
REXCVAR_DEFINE_INT32(ssasr_renut_host50_mark_check, 256, "Banjo/Enhancements",
                     "ssasr_renut_host50 #2 (CaptureDelta compares only the chunks the setters and the dirty tags "
                     "marked): every this many deltas one also compares every chunk; a change the marks missed turns "
                     "the marked compare off for the rest of the run (logged). 0: never checked")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)
namespace ssasr_alloc_census {  // blur_small_alloc.cpp (batch 5 H27)
extern std::atomic<bool> g_on;
std::string Report(double seconds);
}  // namespace ssasr_alloc_census
#endif

namespace ssasr::host50 {

Switches g_switches;
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_count[kCountCount];
std::atomic<bool> g_marks_failed{false};

void LatchSwitches() {
  Switches s;
  const bool all = REXCVAR_GET(ssasr_renut_host50);
  for (int i = 1; i < kItems; ++i)
    s.on[i] = all;
  const std::string off = REXCVAR_GET(ssasr_renut_host50_off);
  for (size_t at = 0; at < off.size();) {
    char *end = nullptr;
    const long v = std::strtol(off.c_str() + at, &end, 10);
    if (end == off.c_str() + at) {
      ++at;
      continue;
    }
    if (v > 0 && v < kItems)
      s.on[v] = false;
    at = size_t(end - off.c_str());
  }
  s.verify = REXCVAR_GET(ssasr_renut_host50_verify);
  s.census = REXCVAR_GET(ssasr_renut_host50_census);
  s.mark_check = REXCVAR_GET(ssasr_renut_host50_mark_check);
#if defined(__SWITCH__) && !defined(RENUT_DIAG_CENSUS_OFF)
  // H27 (only in a census build: a shipping one compiles the census out, and its report with it)
  ssasr_alloc_census::g_on.store(s.census, std::memory_order_relaxed);
#endif
  g_switches = s;
  g_ready.store(true, std::memory_order_release);
}


uint64_t Ticks() {
#if defined(__SWITCH__)
  return armGetSystemTick();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

namespace {
double TickUs() {
#if defined(__SWITCH__)
  return 1e6 / double(armGetSystemTickFreq());
#else
  return 1e6 * double(std::chrono::steady_clock::period::num) / double(std::chrono::steady_clock::period::den);
#endif
}
}  // namespace

void MarksMissed(uint32_t chunk_offset, uint64_t marks) {
  Add(kMarkMisses);
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 24)
    REXLOG_WARN("[renut] ssasr_renut_host50 #2: device chunk +{} (0x{:X}) changed with no mark (marks {:016X}){}",
                chunk_offset, chunk_offset, marks,
                VerifyOn() ? "" : ": the marked compare is off for the rest of the run");
  if (!VerifyOn())
    g_marks_failed.store(true, std::memory_order_relaxed);
}

namespace {
std::atomic<uint64_t> g_tag_count[192];
std::atomic<uint64_t> g_tag_chunks[192];
std::atomic<uint64_t> g_tag_deltas{0}, g_tag_free{0};
// A touch bit's device offset (d3d_native.h MakeTouchTable: the fetch constants' twelve chunks from 0x480, then the
// tail's from 0x2780).
uint32_t TouchBitOffset(uint32_t b) {
  return b < 12 ? 0x480 + 64 * b : 0x2780 + 64 * (b - 12);
}
}  // namespace

void NoteTags(uint64_t states, uint64_t fetch, uint64_t misc, uint64_t unmarked_taken) {
  if (!(states | fetch | misc)) {
    g_tag_free.store(g_tag_free.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  g_tag_deltas.store(g_tag_deltas.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  const uint64_t w[3] = {states, fetch, misc};
  for (int k = 0; k < 3; ++k)
    for (uint64_t m = w[k]; m; m &= m - 1) {
      const int b = k * 64 + std::countr_zero(m);
      g_tag_count[b].store(g_tag_count[b].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      g_tag_chunks[b].store(g_tag_chunks[b].load(std::memory_order_relaxed) | unmarked_taken,
                            std::memory_order_relaxed);
    }
}

namespace {
// The census's N13 part: the deltas with a tag, then the bits by count (word.bit), each with the chunks seen (the
// chunk sets are cumulative: kept from the start).
std::string TagReport() {
  const uint64_t with = g_tag_deltas.exchange(0, std::memory_order_relaxed);
  const uint64_t without = g_tag_free.exchange(0, std::memory_order_relaxed);
  uint64_t n[192];
  for (int b = 0; b < 192; ++b)
    n[b] = g_tag_count[b].exchange(0, std::memory_order_relaxed);
  std::string out = fmt::format("; N13 tags: {:.1f} % of marked deltas with a tag",
                                with + without ? 100.0 * double(with) / double(with + without) : 0.0);
  for (int shown = 0; shown < 12; ++shown) {
    int best = -1;
    for (int b = 0; b < 192; ++b)
      if (n[b] && (best < 0 || n[b] > n[best]))
        best = b;
    if (best < 0)
      break;
    out += fmt::format("{} {}.{} {:.1f} % [", shown ? "," : ":", 2 + best / 64, best % 64,
                       with ? 100.0 * double(n[best]) / double(with) : 0.0);
    const uint64_t chunks = g_tag_chunks[best].load(std::memory_order_relaxed);
    bool first = true;
    for (uint64_t m = chunks; m; m &= m - 1) {
      out += fmt::format("{}{:X}", first ? "" : " ", TouchBitOffset(uint32_t(std::countr_zero(m))));
      first = false;
    }
    out += "]";
    n[best] = 0;
  }
  return out;
}
}  // namespace

std::string Report(double seconds) {
  if (!CensusOn() || seconds <= 0.0)
    return {};
  uint64_t c[kCountCount];
  for (uint32_t i = 0; i < kCountCount; ++i)
    c[i] = g_count[i].exchange(0, std::memory_order_relaxed);
  const double us = TickUs();
  const auto per_s = [seconds](uint64_t v) { return double(v) / seconds; };
  const auto avg = [](uint64_t part, uint64_t whole) { return whole ? double(part) / double(whole) : 0.0; };
  const auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
  const auto ms_s = [&](uint64_t ticks) { return double(ticks) * us / 1000.0 / seconds; };
  const auto ns_each = [&](uint64_t ticks, uint64_t n) { return n ? double(ticks) * us * 1000.0 / double(n) : 0.0; };
  return fmt::format(
      "ssasr_renut_host50 census, a second: #2 {:.0f} deltas, chunks a delta: {:.2f} marked, {:.2f} compared, {:.2f} "
      "taken ({:.2f} without a compare, #45); marks checked {:.0f} times, {} misses{}; #3 {:.1f} % of deltas empty; #5 "
      "{:.2f} constant groups dirty a delta, {:.1f} % the same as the shadow (left out); #6 {:.0f} bytes a delta; "
      "#50 setters {:.0f} calls ({:.2f} ms): SetTexture {:.0f} ({:.0f} ns each, {:.1f} % changed nothing), sampler "
      "states {:.0f} ({:.0f} ns, {:.1f} % nothing), render states {:.0f} ({:.0f} ns), streams {:.0f} ({:.0f} ns, "
      "{:.1f} % nothing); draws {:.0f} ({:.0f} ns each, CaptureDelta {:.0f} ns); pushes {:.0f} ({:.1f} % plain, #1); "
      "stack surfaces bound at {} of {} draws checked (Blur's blur_renut_stack_surface_headers)",
      per_s(c[kDeltas]), avg(c[kChunksMarked], c[kDeltas]), avg(c[kChunksCompared], c[kDeltas]),
      avg(c[kChunksTaken], c[kDeltas]), avg(c[kChunksTakenBlind], c[kDeltas]), per_s(c[kMarkChecks]),
      c[kMarkMisses], g_marks_failed.load(std::memory_order_relaxed) ? " (the marked compare is off)" : "",
      pct(c[kEmptyDeltas], c[kDeltas]), avg(c[kGroupsDirty], c[kDeltas]), pct(c[kGroupsSame], c[kGroupsDirty]),
      avg(c[kDeltaBytes], c[kDeltas]),
      per_s(c[kTextureCalls] + c[kSamplerCalls] + c[kStateCalls] + c[kStreamCalls]),
      ms_s(c[kTextureTicks] + c[kSamplerTicks] + c[kStateTicks] + c[kStreamTicks]), per_s(c[kTextureCalls]),
      ns_each(c[kTextureTicks], c[kTextureCalls]), pct(c[kTextureSame], c[kTextureCalls]), per_s(c[kSamplerCalls]),
      ns_each(c[kSamplerTicks], c[kSamplerCalls]), pct(c[kSamplerSame], c[kSamplerCalls]), per_s(c[kStateCalls]),
      ns_each(c[kStateTicks], c[kStateCalls]), per_s(c[kStreamCalls]), ns_each(c[kStreamTicks], c[kStreamCalls]),
      pct(c[kStreamSame], c[kStreamCalls]), per_s(c[kDrawCalls]), ns_each(c[kDrawTicks], c[kDrawCalls]),
      ns_each(c[kCaptureTicks], c[kDrawCalls]), per_s(c[kPushes]), pct(c[kPlainPushes], c[kPushes]),
      c[kStackSurfaces], c[kStackChecks]) +
         TagReport()
#if defined(__SWITCH__)
         + ssasr_alloc_census::Report(seconds)
#endif
      ;
}

}  // namespace ssasr::host50
