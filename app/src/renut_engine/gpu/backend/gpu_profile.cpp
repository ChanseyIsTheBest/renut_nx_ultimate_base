/**
 * @file    gpu/backend/gpu_profile.cpp
 */
#include "renut_engine/gpu/banjo_renut_record.h"  // banjo-nx (2026-11-21): the deko3d command stage
#include "renut_engine/gpu/backend/gpu_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <plume_render_interface.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "profiler/profiler.h"
#include "renut_engine/gpu/backend/backend.h"
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>  // vivapinata-nx (2026-11-12): renut_gpu_profile_counters
#endif
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_deko3d.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/perf/frame_stats.h"

namespace renut::gpu {
std::string DroppedDrawsReport();
}

extern "C" {
const char *RexAppBuildStamp(void) __attribute__((weak));
bool RexSwitchPerfStacksActive(void) __attribute__((weak));
}

REXCVAR_DEFINE_DOUBLE(renut_gpu_profile, 0.0, "Nuts&Bolts/Debug",
                      "Every this many seconds, log the GPU's time per frame by kind of work "
                      "(draws per target, resolves, mip levels, present). 0 = off.");
// vivapinata-nx (2026-11-12, diagnostic, deko3d): beside each mark's timestamp the GPU's count of pixel shader
// invocations and its Zcull statistics, so each line also says how many pixels it shaded and what Zcull rejected.
REXCVAR_DEFINE_BOOL(renut_gpu_profile_counters, false, "Nuts&Bolts/Debug",
                    "With renut_gpu_profile on deko3d: each line also gives the pixel shader invocations a frame (M) and "
                    "the change of the GPU's four Zcull statistics words a frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (package 28b): the passes split by pixel shader (render/draw.cpp, ShaderProfileLabel), more lines.
REXCVAR_DECLARE(bool, banjo_renut_diag_gpu_shaders);

// renut-ultimate (from deadpool-nx v1.0.2's "Capture Frame Diag" button, which lives in its quick menu): a trigger
// that needs no menu. 0 = never: nothing is armed, and every FrameDiagActive() check stays false.
REXCVAR_DEFINE_DOUBLE(deadpool_frame_diag_at_s, 0.0, "Nuts&Bolts/Debug",
                      "deadpool-nx: seconds after the first frame at which one frame's diagnostic is captured to "
                      "logs/frame_diag_<time>.log (GPU time by pass and shader, deko3d counters and command trace, every "
                      "pass, clear, resolve and draw); 0 = never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu::gpu_profile {

bool g_recording_timed = false;
std::atomic<bool> g_frame_diag_recording{false};

namespace {

// Timestamps one frame may write; past them its work stays with the last
// kind marked.
constexpr uint32_t kMaxMarks = 2048;  // the base's: a diagnostic frame past it counts the marks it dropped
constexpr uint32_t kEndOfList = 0;  // a list's last timestamp: no span follows
constexpr uint32_t kOther = 1;      // a list's start, before any kind is marked

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters): the pixel shader invocations (an entry a mark) and the
  // Zcull statistics (4 entries a mark: the report's size is not documented, so room for four).
  std::unique_ptr<plume::RenderQueryPool> frag_pool, zcull_pool;
  bool open = false;             // the slot's frame has begun recording
  bool active = false;           // ... and is timed (regular profile and/or diag capture)
  bool regular_profile = false;  // renut_gpu_profile > 0.0
  bool diag_capture = false;     // one-shot Capture Frame Diag
  uint32_t count = 0;
  uint32_t dropped_marks = 0;
  uint32_t list = 0;
  uint32_t current = kEndOfList;
  std::array<uint32_t, kMaxMarks> labels{};
  std::array<uint32_t, kMaxMarks> lists{};
};
std::array<Slot, kFramesInFlight> g_slots;

std::vector<std::string> g_names = {"(list end)", "other"};
std::unordered_map<std::string, uint32_t> g_ids = {{"(list end)", kEndOfList}, {"other", kOther}};

// Since the last report: GPU nanoseconds per kind, and frames.
std::vector<uint64_t> g_totals;
std::vector<uint64_t> g_draws;  // banjo-nx (package 28b): draws per kind (CountDraw)
std::vector<uint64_t> g_frags;  // vivapinata-nx (2026-11-12): pixel shader invocations per kind
std::vector<std::array<uint64_t, 8>> g_zcull;  // vivapinata-nx (2026-11-12): Zcull statistics words per kind
bool CountersOn() {
#if defined(PLUME_DEKO3D)
  static const bool on = REXCVAR_GET(renut_gpu_profile_counters) && IsDeko3D();
  return on;
#else
  return false;
#endif
}
uint64_t g_frames = 0;
std::chrono::steady_clock::time_point g_last_report = std::chrono::steady_clock::now();

enum class DiagState : uint8_t { Idle = 0, Armed = 1, Recording = 2, WaitingGpu = 3 };
std::atomic<uint8_t> g_diag_state{0};
std::mutex g_diag_mutex;
std::string g_diag_status;

// Render-thread-only one-shot capture buffers (zero overhead when idle).
uint64_t g_diag_frame_ordinal = 0;
uint32_t g_diag_draw_ordinal = 0;
uint32_t g_diag_events_dropped = 0;
constexpr size_t kMaxDiagEvents = 8192;
std::vector<std::string> g_diag_events;
std::vector<uint64_t> g_diag_label_draws;
std::string g_diag_deko3d_trace;
bool g_diag_prev_deko_trace = false;

#if defined(PLUME_DEKO3D)
struct DekoStatsSnap {
  uint64_t draws = 0, windowDraws = 0, pipelineBinds = 0, stateBinds = 0;
  uint64_t textureRuns = 0, textureSkips = 0;
  uint64_t barriersPrimitives = 0, barriersFragments = 0, barriersFull = 0, barriersImageInvalidate = 0;
  uint64_t copyBuffer = 0, copyUpload = 0, copyReadback = 0, copyImage = 0, copyResolve = 0, copyBlit = 0;
  uint64_t depthResolvesBound = 0, depthResolvesDeferred = 0, depthResolvesLater = 0;
  uint64_t descriptorWrites = 0, descriptorCacheHits = 0;
  uint64_t shaderCompiles = 0, compileWaits = 0, variantFallbacks = 0;
  uint64_t imageBytes = 0, bufferBytes = 0, codeBytes = 0, commandBytes = 0;
};
DekoStatsSnap g_diag_dk_start{}, g_diag_dk_end{};

DekoStatsSnap SnapshotDekoStats() {
  const plume::Deko3DStats &st = plume::GetDeko3DStats();
  DekoStatsSnap s;
  s.draws = st.draws.load(std::memory_order_relaxed);
  s.windowDraws = st.windowDraws.load(std::memory_order_relaxed);
  s.pipelineBinds = st.pipelineBinds.load(std::memory_order_relaxed);
  s.stateBinds = st.stateBinds.load(std::memory_order_relaxed);
  s.textureRuns = st.textureRuns.load(std::memory_order_relaxed);
  s.textureSkips = st.textureSkips.load(std::memory_order_relaxed);
  s.barriersPrimitives = st.barriersPrimitives.load(std::memory_order_relaxed);
  s.barriersFragments = st.barriersFragments.load(std::memory_order_relaxed);
  s.barriersFull = st.barriersFull.load(std::memory_order_relaxed);
  s.barriersImageInvalidate = st.barriersImageInvalidate.load(std::memory_order_relaxed);
  s.copyBuffer = st.copyBuffer.load(std::memory_order_relaxed);
  s.copyUpload = st.copyUpload.load(std::memory_order_relaxed);
  s.copyReadback = st.copyReadback.load(std::memory_order_relaxed);
  s.copyImage = st.copyImage.load(std::memory_order_relaxed);
  s.copyResolve = st.copyResolve.load(std::memory_order_relaxed);
  s.copyBlit = st.copyBlit.load(std::memory_order_relaxed);
  s.depthResolvesBound = st.depthResolvesBound.load(std::memory_order_relaxed);
  s.depthResolvesDeferred = st.depthResolvesDeferred.load(std::memory_order_relaxed);
  s.depthResolvesLater = st.depthResolvesLater.load(std::memory_order_relaxed);
  s.descriptorWrites = st.descriptorWrites.load(std::memory_order_relaxed);
  s.descriptorCacheHits = st.descriptorCacheHits.load(std::memory_order_relaxed);
  s.shaderCompiles = st.shaderCompiles.load(std::memory_order_relaxed);
  s.compileWaits = st.compileWaits.load(std::memory_order_relaxed);
  s.variantFallbacks = st.variantFallbacks.load(std::memory_order_relaxed);
  s.imageBytes = st.imageBytes.load(std::memory_order_relaxed);
  s.bufferBytes = st.bufferBytes.load(std::memory_order_relaxed);
  s.codeBytes = st.codeBytes.load(std::memory_order_relaxed);
  s.commandBytes = st.commandBytes.load(std::memory_order_relaxed);
  return s;
}
#endif

std::string WriteDiagCaptureFile(const std::vector<uint64_t> *frame_ns, bool has_timestamps,
                                 uint32_t marks_used, uint32_t marks_dropped) {
  std::error_code ec;
  const std::filesystem::path log_dir = rex::filesystem::GetExecutableFolder() / "logs";
  std::filesystem::create_directories(log_dir, ec);

  const std::time_t now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm_now{};
#if defined(_WIN32)
  localtime_s(&tm_now, &now_t);
#else
  localtime_r(&now_t, &tm_now);
#endif
  char filename[64];
  std::snprintf(filename, sizeof(filename), "frame_diag_%04d%02d%02d_%02d%02d%02d.log",
                tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  const std::filesystem::path out_path = log_dir / filename;

  FILE *f = std::fopen(out_path.string().c_str(), "w");
  if (!f) {
    return "Failed to open " + out_path.string();
  }

  const char *stamp = RexAppBuildStamp ? RexAppBuildStamp() : "unknown";
  const bool stacks_on = RexSwitchPerfStacksActive ? RexSwitchPerfStacksActive() : false;
  const auto snap = renut::frame_stats::Get();
  const auto proc_cpu = banjo::profiler::QueryProcessCpuUsage();
#if defined(PLUME_DEKO3D)  // renut-ultimate: banjo_renut_deko3d.cpp is built for the deko3d back end only
  const std::string d3d_report = banjo::renut_gpu::deko3d::Report();
#else
  const std::string d3d_report;
#endif
  const std::string dropped = renut::gpu::DroppedDrawsReport();
  const std::string threads = banjo::profiler::ThreadCpuUsage();

  std::fprintf(f, "=== 1. SUMMARY ===\n");
  std::fprintf(f, "Build: %s\n", stamp ? stamp : "unknown");
  std::fprintf(f, "Captured diag frame #%llu\n", static_cast<unsigned long long>(g_diag_frame_ordinal));
  std::fprintf(f, "Stack profiler (stacks_profile.flag): %s\n", stacks_on ? "ON" : "off");
  const std::string rt_ms = snap.render_thread_known ? fmt::format("{:.2f} ms", snap.render_thread_ms) : "n/a";
  const std::string gpu_ms = snap.gpu_known ? fmt::format("{:.2f} ms", snap.gpu_ms) : "n/a";
  std::fprintf(f, "Last second: %.1f fps, %.2f ms a frame (worst %.2f), game thread CPU %.2f ms, render thread %s, GPU %s\n",
               snap.fps, snap.frame_ms, snap.worst_ms, snap.cpu_ms, rt_ms.c_str(), gpu_ms.c_str());
  if (proc_cpu.valid) {
    std::fprintf(f, "Process CPU: %.1f%% of one core (%.1f%% of four cores)\n",
                 proc_cpu.pct_one_core, proc_cpu.pct_four_cores);
  }
  if (!dropped.empty()) {
    std::fprintf(f, "DroppedDraws: %s\n", dropped.c_str());
  }
  if (!threads.empty()) {
    std::fprintf(f, "Thread CPU: %s\n", threads.c_str());
  }

  std::fprintf(f, "\n=== 2. CAPTURED FRAME GPU PROFILE (HARDWARE TIMESTAMPS) ===\n");
  if (!has_timestamps || !frame_ns) {
    std::fprintf(f, "GPU timestamp query pool unavailable or empty (marks_used=%u)\n", marks_used);
  } else {
    uint64_t total_ns = 0;
    std::unordered_map<std::string, uint64_t> pass_ns;
    std::unordered_map<std::string, uint64_t> pass_draws;
    std::vector<std::pair<uint64_t, uint32_t>> sorted_labels;
    for (uint32_t i = 0; i < frame_ns->size(); ++i) {
      const uint64_t ns = (*frame_ns)[i];
      const uint64_t dc = i < g_diag_label_draws.size() ? g_diag_label_draws[i] : 0;
      if (!ns && !dc)
        continue;
      total_ns += ns;
      sorted_labels.emplace_back(ns, i);
      const std::string &full = g_names[i];
      const size_t sep = full.find(" | ");
      const std::string prefix = (sep == std::string::npos) ? full : full.substr(0, sep);
      pass_ns[prefix] += ns;
      pass_draws[prefix] += dc;
    }
    std::vector<std::pair<uint64_t, std::string>> sorted_passes;
    sorted_passes.reserve(pass_ns.size());
    for (const auto &kv : pass_ns) {
      sorted_passes.emplace_back(kv.second, kv.first);
    }
    std::sort(sorted_passes.begin(), sorted_passes.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    std::sort(sorted_labels.begin(), sorted_labels.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    std::fprintf(f, "Captured frame GPU total: %.3f ms (marks used %u/%u, dropped %u)\n",
                 double(total_ns) / 1e6, marks_used, kMaxMarks, marks_dropped);
    std::fprintf(f, "-- 2A. By Pass / Target --\n");
    for (const auto &p : sorted_passes) {
      const uint64_t dc = pass_draws[p.second];
      std::fprintf(f, "  %7.3f ms  %-42s  (%llu draws)\n",
                   double(p.first) / 1e6, p.second.c_str(), static_cast<unsigned long long>(dc));
    }
    std::fprintf(f, "-- 2B. By Shader / Pass Detail --\n");
    for (const auto &item : sorted_labels) {
      const uint32_t idx = item.second;
      const uint64_t dc = idx < g_diag_label_draws.size() ? g_diag_label_draws[idx] : 0;
      std::fprintf(f, "  %7.3f ms  %4llu draws  %s\n",
                   double(item.first) / 1e6, static_cast<unsigned long long>(dc), g_names[idx].c_str());
    }
  }

  std::fprintf(f, "\n=== 3. CAPTURED FRAME DEKO3D STATS (THIS FRAME DELTA + LIVE MEMORY) ===\n");
#if defined(PLUME_DEKO3D)
  if (IsDeko3D()) {
    const auto d = [](uint64_t e, uint64_t b) { return e >= b ? (e - b) : 0ull; };
    std::fprintf(f,
                 "This frame: draws=%llu (windowDraws=%llu), pipelineBinds=%llu, stateBinds=%llu, "
                 "boundTextures read=%llu skipped=%llu\n",
                 static_cast<unsigned long long>(d(g_diag_dk_end.draws, g_diag_dk_start.draws)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.windowDraws, g_diag_dk_start.windowDraws)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.pipelineBinds, g_diag_dk_start.pipelineBinds)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.stateBinds, g_diag_dk_start.stateBinds)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.textureRuns, g_diag_dk_start.textureRuns)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.textureSkips, g_diag_dk_start.textureSkips)));
    std::fprintf(f,
                 "This frame barriers: primitives=%llu, fragments=%llu, full=%llu, invalidatingTextures=%llu\n",
                 static_cast<unsigned long long>(d(g_diag_dk_end.barriersPrimitives, g_diag_dk_start.barriersPrimitives)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.barriersFragments, g_diag_dk_start.barriersFragments)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.barriersFull, g_diag_dk_start.barriersFull)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.barriersImageInvalidate, g_diag_dk_start.barriersImageInvalidate)));
    std::fprintf(f,
                 "This frame depth decompresses: bound=%llu, deferred=%llu, later=%llu\n",
                 static_cast<unsigned long long>(d(g_diag_dk_end.depthResolvesBound, g_diag_dk_start.depthResolvesBound)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.depthResolvesDeferred, g_diag_dk_start.depthResolvesDeferred)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.depthResolvesLater, g_diag_dk_start.depthResolvesLater)));
    std::fprintf(f,
                 "This frame copies/2D: buffer=%llu, upload=%llu, readback=%llu, image=%llu, resolve=%llu, blit=%llu\n",
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyBuffer, g_diag_dk_start.copyBuffer)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyUpload, g_diag_dk_start.copyUpload)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyReadback, g_diag_dk_start.copyReadback)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyImage, g_diag_dk_start.copyImage)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyResolve, g_diag_dk_start.copyResolve)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.copyBlit, g_diag_dk_start.copyBlit)));
    std::fprintf(f,
                 "This frame descriptors/uam: descWrites=%llu (cacheHits=%llu), uamCompiles=%llu, compileWaits=%llu, variantFallbacks=%llu\n",
                 static_cast<unsigned long long>(d(g_diag_dk_end.descriptorWrites, g_diag_dk_start.descriptorWrites)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.descriptorCacheHits, g_diag_dk_start.descriptorCacheHits)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.shaderCompiles, g_diag_dk_start.shaderCompiles)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.compileWaits, g_diag_dk_start.compileWaits)),
                 static_cast<unsigned long long>(d(g_diag_dk_end.variantFallbacks, g_diag_dk_start.variantFallbacks)));
    std::fprintf(f,
                 "Live GPU memory at end of frame: images=%llu MB, buffers=%llu MB, code=%llu KB, commands=%llu KB\n",
                 static_cast<unsigned long long>(g_diag_dk_end.imageBytes >> 20),
                 static_cast<unsigned long long>(g_diag_dk_end.bufferBytes >> 20),
                 static_cast<unsigned long long>(g_diag_dk_end.codeBytes >> 10),
                 static_cast<unsigned long long>(g_diag_dk_end.commandBytes >> 10));
  }
#endif
  if (!d3d_report.empty()) {
    std::fprintf(f, "Cumulative session %s\n", d3d_report.c_str());
  }

  std::fprintf(f, "\n=== 4. CAPTURED FRAME DEKO3D COMMAND STREAM TRACE ===\n");
  std::fprintf(f,
               "Legend: B<P/F/X>+causes(r=read,c=copy,C=colour,d=depth,o=other,b=buffer,k=compute,m=merged,+i=texCache) "
               "D=draw X=dispatch T=target Cc/Cd=clear Z=depthDecompress K=copyEngine I=descInvalidate E=listEnd\n");
  if (g_diag_deko3d_trace.empty()) {
    std::fprintf(f, "(empty)\n");
  } else {
    for (size_t pos = 0; pos < g_diag_deko3d_trace.size(); pos += 160) {
      const size_t len = std::min<size_t>(160, g_diag_deko3d_trace.size() - pos);
      std::fprintf(f, "%.*s\n", int(len), g_diag_deko3d_trace.data() + pos);
    }
  }

  std::fprintf(f, "\n=== 5. CAPTURED FRAME PASS / CLEAR / RESOLVE / DRAW LOG ===\n");
  std::fprintf(f, "Events captured: %zu (dropped: %u, draws: %u)\n",
               g_diag_events.size(), g_diag_events_dropped, g_diag_draw_ordinal);
  for (const std::string &ev : g_diag_events) {
    std::fprintf(f, "%s\n", ev.c_str());
  }

  std::fclose(f);
  REXLOG_INFO("[diag] captured frame diagnostic: {}", out_path.string());
  return std::string("Saved: logs/") + filename;
}

void Write(VideoState &s, Slot &slot, uint32_t label) {
  s.command_list->writeTimestamp(slot.pool.get(), slot.count);
#if defined(PLUME_DEKO3D)
  if (slot.frag_pool) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
    banjo::renut_gpu::record::ReportCounter(s.command_list, slot.frag_pool.get(), slot.count, 10);  // FragmentShaderInvocations
    banjo::renut_gpu::record::ReportCounter(s.command_list, slot.zcull_pool.get(), slot.count * 4, 3);  // ZcullStats
  }
#endif
  slot.labels[slot.count] = label;
  slot.lists[slot.count] = slot.list;
  ++slot.count;
  slot.current = label;
}

void Report(double interval) {
  const auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<double>(now - g_last_report).count() < interval || !g_frames)
    return;
  g_last_report = now;
  std::vector<std::pair<uint64_t, uint32_t>> kinds;
  uint64_t total = 0;
  for (uint32_t i = 0; i < g_totals.size(); ++i) {
    if (g_totals[i]) {
      kinds.emplace_back(g_totals[i], i);
      total += g_totals[i];
    }
  }
  std::sort(kinds.rbegin(), kinds.rend());
  const double per_frame = 1e6 * double(g_frames);
  std::string lines;
  const size_t shown = REXCVAR_GET(banjo_renut_diag_gpu_shaders) ? 60 : 24;
  for (size_t i = 0; i < kinds.size() && i < shown; ++i) {
    const uint32_t k = kinds[i].second;
    const uint64_t draws = k < g_draws.size() ? g_draws[k] : 0;
    lines += fmt::format("\n  {:7.3f} ms  {}", double(kinds[i].first) / per_frame, g_names[k]);
    if (draws)
      lines += fmt::format("  ({:.1f} draws a frame)", double(draws) / double(g_frames));
    if (CountersOn() && k < g_frags.size()) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
      const auto &z = g_zcull[k];
      lines += fmt::format("  [{:.3f} M pixels shaded; zcull {} {} {} {} {} {} {} {}]", double(g_frags[k]) / 1e6 / double(g_frames),
                           z[0] / g_frames, z[1] / g_frames, z[2] / g_frames, z[3] / g_frames, z[4] / g_frames,
                           z[5] / g_frames, z[6] / g_frames, z[7] / g_frames);
    }
  }
  RGPU_INFO("GPU profile: {:.3f} ms per frame over {} frames{}", double(total) / per_frame,
            g_frames, lines);
  std::fill(g_totals.begin(), g_totals.end(), 0);
  std::fill(g_draws.begin(), g_draws.end(), 0);
  std::fill(g_frags.begin(), g_frags.end(), 0);
  std::fill(g_zcull.begin(), g_zcull.end(), std::array<uint64_t, 8>{});
  g_frames = 0;
}

} // namespace

uint32_t Label(std::string_view name) {
  auto it = g_ids.find(std::string(name));
  if (it != g_ids.end())
    return it->second;
  const uint32_t id = uint32_t(g_names.size());
  g_names.emplace_back(name);
  g_ids.emplace(std::string(name), id);
  return id;
}

std::string_view Name(uint32_t label) {
  return label < g_names.size() ? std::string_view(g_names[label]) : std::string_view("?");
}

void RequestFrameDiagCapture() {
  std::lock_guard<std::mutex> lock(g_diag_mutex);
  uint8_t expected = static_cast<uint8_t>(DiagState::Idle);
  if (g_diag_state.compare_exchange_strong(expected, static_cast<uint8_t>(DiagState::Armed),
                                           std::memory_order_acq_rel)) {
    g_diag_status = "Armed: capturing next frame...";
  }
}

std::string FrameDiagStatus() {
  std::lock_guard<std::mutex> lock(g_diag_mutex);
  return g_diag_status;
}

void FrameDiagNoteEvent(std::string_view line) {
  if (g_diag_events.size() < kMaxDiagEvents) {
    g_diag_events.emplace_back(line);
  } else {
    ++g_diag_events_dropped;
  }
}

uint32_t FrameDiagNextDrawOrdinal() {
  return g_diag_draw_ordinal++;
}

void CountDraw(VideoState &s, uint32_t label) {
  // Counted when recorded; reported with the frames the GPU profile collected (both every frame).
  const Slot &slot = g_slots[s.frame];
  if (slot.regular_profile) {
    if (g_draws.size() <= label)
      g_draws.resize(label + 1);
    ++g_draws[label];
  }
  if (slot.diag_capture) {
    if (g_diag_label_draws.size() <= label)
      g_diag_label_draws.resize(label + 1);
    ++g_diag_label_draws[label];
  }
}

void Mark(VideoState &s, uint32_t label) {
  Slot &slot = g_slots[s.frame];
  if (!slot.active || !s.command_list || label == slot.current)
    return;
  if (slot.count + 2 >= kMaxMarks) {
    ++slot.dropped_marks;
    return;
  }
  Write(s, slot, label);
}

void BeginList(VideoState &s) {
  Slot &slot = g_slots[s.frame];
  if (!slot.open) {
    slot.open = true;
    slot.count = 0;
    slot.dropped_marks = 0;
    slot.list = 0;
    slot.diag_capture = false;
    slot.regular_profile = REXCVAR_GET(renut_gpu_profile) > 0.0 && s.device->getCapabilities().queryPools;
    // banjo-nx (2026-10-19, GPU): with the per-shader breakdown (a timestamp at nearly every draw), one frame in
    // kShaderSample is timed; the report's averages are over the timed frames (draws are counted in them only).
    static uint32_t frames_seen = 0;
    // banjo-nx (2026-10-28, per-draw night): 8 (was 4): the timed frames' marks and labels cost the render thread.
    constexpr uint32_t kShaderSample = 8;
    if (slot.regular_profile && REXCVAR_GET(banjo_renut_diag_gpu_shaders) && (frames_seen++ % kShaderSample) != 0)
      slot.regular_profile = false;

    static const double diag_at_s = REXCVAR_GET(deadpool_frame_diag_at_s);
    if (diag_at_s > 0.0) {  // armed once, that many seconds after the first list
      static const auto first = std::chrono::steady_clock::now();
      static bool fired = false;
      if (!fired && std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() >= diag_at_s) {
        fired = true;
        RequestFrameDiagCapture();
      }
    }
    uint8_t expected = static_cast<uint8_t>(DiagState::Armed);
    if (diag_at_s > 0.0 &&
        g_diag_state.compare_exchange_strong(expected, static_cast<uint8_t>(DiagState::Recording),
                                             std::memory_order_acq_rel)) {
      slot.diag_capture = true;
      ++g_diag_frame_ordinal;
      g_diag_draw_ordinal = 0;
      g_diag_events_dropped = 0;
      g_diag_events.clear();
      g_diag_events.reserve(4096);
      g_diag_label_draws.clear();
      g_diag_deko3d_trace.clear();
#if defined(PLUME_DEKO3D)
      if (IsDeko3D()) {
        g_diag_dk_start = SnapshotDekoStats();
        g_diag_prev_deko_trace = plume::g_deko3dTrace;
        if (!g_diag_prev_deko_trace) {
          plume::Deko3DTraceTake();
          plume::g_deko3dTrace = true;
        }
      }
#endif
      {
        std::lock_guard<std::mutex> lock(g_diag_mutex);
        g_diag_status = "Recording frame...";
      }
      g_frame_diag_recording.store(true, std::memory_order_release);
    }

    slot.active = (slot.regular_profile || slot.diag_capture) && s.device->getCapabilities().queryPools;
    if (slot.active && !slot.pool) {
      slot.pool = s.device->createQueryPool(kMaxMarks);
      slot.active = slot.pool != nullptr;
      if (slot.active && CountersOn()) {  // vivapinata-nx (2026-11-12, renut_gpu_profile_counters)
        slot.frag_pool = s.device->createQueryPool(kMaxMarks);
        slot.zcull_pool = s.device->createQueryPool(kMaxMarks * 4);
        if (!slot.frag_pool || !slot.zcull_pool)
          slot.frag_pool.reset(), slot.zcull_pool.reset();
      }
    }
    if (!slot.active)
      slot.regular_profile = false;  // no pool: no draws counted, as the base's CountDraw (it read slot.active)
    // Vulkan queries are reset before they are written again.
    if (slot.active)
      s.command_list->resetQueryPool(slot.pool.get(), 0, kMaxMarks);
  }
  g_recording_timed = slot.active;  // banjo-nx (2026-10-19, render-thread proposal 7)
  if (slot.active && slot.count + 2 < kMaxMarks)
    Write(s, slot, kOther);
}

void EndList(VideoState &s, bool last_in_frame) {
  Slot &slot = g_slots[s.frame];
  if (slot.active && s.command_list) {
    if (slot.count < kMaxMarks)
      Write(s, slot, kEndOfList);
    ++slot.list;
    // Vulkan returns no results while any query of the pool is unwritten.
    // banjo-nx (package 28b): unless the pool reads only the written ones (plume's queryResults(count)).
    if (last_in_frame && IsVulkan() && !slot.pool->partialResults()) {
      for (uint32_t i = slot.count; i < kMaxMarks; ++i)
        s.command_list->writeTimestamp(slot.pool.get(), i);
    }
  }
  if (last_in_frame && slot.diag_capture) {
    g_frame_diag_recording.store(false, std::memory_order_release);
#if defined(PLUME_DEKO3D)
    if (IsDeko3D()) {
      g_diag_dk_end = SnapshotDekoStats();
      g_diag_deko3d_trace = plume::Deko3DTraceTake();
      if (!g_diag_prev_deko_trace) {
        plume::g_deko3dTrace = false;
      }
    }
#endif
    g_diag_state.store(static_cast<uint8_t>(DiagState::WaitingGpu), std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_diag_mutex);
    g_diag_status = "Waiting for GPU timestamps...";
  }
}

void Collect(VideoState &s, uint32_t index) {
  (void)s;
  Slot &slot = g_slots[index];
  const bool regular = slot.regular_profile;
  const bool diag = slot.diag_capture;
  const bool has_timestamps = slot.active && slot.count > 1;
  const uint32_t marks_used = slot.count;
  const uint32_t marks_dropped = slot.dropped_marks;
  slot.open = false;
  slot.active = false;
  slot.regular_profile = false;
  slot.diag_capture = false;
  slot.dropped_marks = 0;
  if (!has_timestamps && !diag)
    return;

  const uint64_t *ns = nullptr;
  if (has_timestamps) {
    slot.pool->queryResults(marks_used);  // banjo-nx: the written ones only, where the backend can
    ns = slot.pool->getResults();
  }

  if (regular && ns) {
    if (g_totals.size() < g_names.size())
      g_totals.resize(g_names.size());
    for (uint32_t i = 0; i + 1 < marks_used; ++i) {
      if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
        continue;
      g_totals[slot.labels[i]] += ns[i + 1] - ns[i];
    }
#if defined(PLUME_DEKO3D)
    // vivapinata-nx (2026-11-12, renut_gpu_profile_counters): each span's change of the counters, to its label.
    if (slot.frag_pool) {
      const volatile uint64_t *f = plume::Deko3DQueryPoolWords(slot.frag_pool.get());
      const volatile uint64_t *z = plume::Deko3DQueryPoolWords(slot.zcull_pool.get());
      if (f && z) {
        if (g_frags.size() < g_names.size()) {
          g_frags.resize(g_names.size());
          g_zcull.resize(g_names.size());
        }
        for (uint32_t i = 0; i + 1 < marks_used; ++i) {
          if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
            continue;
          const uint32_t k = slot.labels[i];
          const uint64_t a = f[i * 2], b = f[(i + 1) * 2];
          if (b >= a)
            g_frags[k] += b - a;
          // the four 16-byte entries of a mark's Zcull report, read as eight 32-bit words
          for (int w = 0; w < 8; ++w) {
            const uint64_t wa = z[i * 8 + w / 2] >> (32 * (w & 1)) & 0xFFFFFFFFull;
            const uint64_t wb = z[(i + 1) * 8 + w / 2] >> (32 * (w & 1)) & 0xFFFFFFFFull;
            if (wb >= wa)
              g_zcull[k][w] += wb - wa;
          }
        }
      }
    }
#endif
    ++g_frames;
    Report(REXCVAR_GET(renut_gpu_profile));
  }

  if (diag) {
    std::vector<uint64_t> frame_ns;
    if (ns && marks_used > 1) {
      frame_ns.assign(g_names.size(), 0);
      for (uint32_t i = 0; i + 1 < marks_used; ++i) {
        if (slot.labels[i] == kEndOfList || slot.lists[i] != slot.lists[i + 1] || ns[i + 1] <= ns[i])
          continue;
        const uint32_t lbl = slot.labels[i];
        if (lbl < frame_ns.size())
          frame_ns[lbl] += ns[i + 1] - ns[i];
      }
    }
    const std::string status = WriteDiagCaptureFile(ns ? &frame_ns : nullptr, has_timestamps && ns != nullptr,
                                                    marks_used, marks_dropped);
    std::vector<std::string>().swap(g_diag_events);
    std::vector<uint64_t>().swap(g_diag_label_draws);
    std::string().swap(g_diag_deko3d_trace);
    {
      std::lock_guard<std::mutex> lock(g_diag_mutex);
      g_diag_status = status;
    }
    g_diag_state.store(static_cast<uint8_t>(DiagState::Idle), std::memory_order_release);
  }
}

} // namespace renut::gpu::gpu_profile
