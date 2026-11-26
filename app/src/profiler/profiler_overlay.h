// banjo-nx: the on-screen profiler, ported from UnleashedRecomp's DrawProfiler() (gpu/video.cpp).
//
// Same layout: a "Frame Time" graph of every series over the last 256 UI frames, then the current and
// average value of each, then renderer/device facts. Unleashed plots with ImPlot; the SDK's ImGui
// (1.92) is newer than any ImPlot release Unleashed carries, so the graph is drawn with the window's
// draw list instead. F5 on PC (F1 is reNut's FPS overlay); banjo_show_profiler everywhere.
#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"
#include "renut_engine/overlays/app_imgui.h"  // renut-ultimate (port 66)
#include "renut_engine/perf/frame_stats.h"
#include "profiler/profiler.h"
#if REX_PLATFORM_SWITCH
#include "profiler/switch_cpu_profiler.h"
extern "C" bool RexSwitchPerfStacksActive(void);
#endif

REXCVAR_DECLARE(bool, banjo_show_profiler);

class ProfilerOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ProfilerOverlayDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
    rex::ui::RegisterBind("bind_profiler", "F5", "Toggle CPU/GPU profiler", [] {
      rex::cvar::SetFlagByName("banjo_show_profiler", REXCVAR_GET(banjo_show_profiler) ? "false" : "true");
    });
  }
  ~ProfilerOverlayDialog() override { rex::ui::UnregisterBind("bind_profiler"); }

  // blur-nx (2026-11-18, #27): only while the window shows (its graphs then fill from that frame on).
  bool WantsFrame() const override { return REXCVAR_GET(banjo_show_profiler); }

  void OnDraw(ImGuiIO& io) override {
    using banjo::profiler::Series;
    constexpr size_t kSeries = static_cast<size_t>(Series::kCount);
    constexpr size_t kHistory = 256;

    // Sample every series once per UI frame, as Unleashed's window did.
    for (size_t s = 0; s < kSeries; ++s) {
      const auto series = static_cast<Series>(s);
      history_[s][index_] = banjo::profiler::HasValue(series)
                                ? static_cast<float>(banjo::profiler::Current(series))
                                : 0.0f;
    }
    index_ = (index_ + 1) % kHistory;

    if (!REXCVAR_GET(banjo_show_profiler)) return;
    if (!AppImGuiReady()) return;  // renut-ultimate (port 66)

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 580.0f, 20.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Profiler", &open)) {
      DrawGraph();

      ImGui::Separator();
      for (size_t s = 0; s < kSeries; ++s) {
        const auto series = static_cast<Series>(s);
        if (!banjo::profiler::HasValue(series)) {
          ImGui::TextDisabled("%-28s n/a", banjo::profiler::SeriesName(series));
          continue;
        }
        const double cur = banjo::profiler::Current(series);
        const double avg = Average(s);
        ImGui::TextColored(Color(s), "%-28s", banjo::profiler::SeriesName(series));
        ImGui::SameLine();
        if (series == Series::kLimiterWait || series == Series::kRenderThread) {
          ImGui::Text("%7.2f ms  (avg %7.2f ms)", cur, avg);
        } else {
          ImGui::Text("%7.2f ms  (avg %7.2f ms, %5.1f FPS)", cur, avg, avg > 0.0 ? 1000.0 / avg : 0.0);
        }
      }

      ImGui::Separator();
      ImGui::Text("Renderer: %s", banjo::profiler::RendererApi().c_str());
      const std::string device = banjo::profiler::RendererDevice();
      if (!device.empty()) ImGui::Text("Device: %s", device.c_str());

      // CPU use is measured since the previous query, so ask once a second (Unleashed: once per report).
      if (io.DeltaTime > 0.0f) threads_age_ += io.DeltaTime;
      if (threads_age_ >= 1.0f) {
        threads_age_ = 0.0f;
        const std::string threads = banjo::profiler::ThreadCpuUsage();
        if (!threads.empty()) last_threads_ = threads;
      }
      if (!last_threads_.empty() && ImGui::TreeNode("Thread CPU use")) {
        ImGui::TextWrapped("%s", last_threads_.c_str());
        ImGui::TreePop();
      }
    }
    ImGui::End();
    if (!open) rex::cvar::SetFlagByName("banjo_show_profiler", "false");

    // Bottom-left detailed frame pacing / vblanks / process CPU / waits panel (NFSMW-NX / LMNX style)
    ImGui::SetNextWindowSize(ImVec2(680.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20.0f, std::max(20.0f, io.DisplaySize.y - 315.0f)), ImGuiCond_FirstUseEver);
    bool pacing_open = true;
    if (ImGui::Begin("Profiler (Renut)", &pacing_open)) {
      if (io.DeltaTime > 0.0f) pacing_age_ += io.DeltaTime;
      if (pacing_age_ >= 0.25f) {
        pacing_age_ = 0.0f;
        last_pacing_ = renut::frame_stats::GetPacingSnapshot();
      }
      const auto& p = last_pacing_;
      ImGui::Text("Frame pacing (last %d game frames): best %.1f ms, worst %.1f ms, median %.1f ms",
                  p.count, p.best_ms, p.worst_ms, p.median_ms);
      ImGui::Text("  average %.1f FPS, 1%% low %.1f FPS (average of the %d slowest), stutters (> 2x median): %d",
                  p.avg_fps, p.low1_fps, p.low1_count, p.stutters);
      ImGui::Text("  vblanks per frame:  1: %3d%%   2: %3d%%   3: %3d%%   4: %3d%%   5+: %3d%%",
                  p.vblank_pct[0], p.vblank_pct[1], p.vblank_pct[2], p.vblank_pct[3], p.vblank_pct[4]);
#if REX_PLATFORM_SWITCH
      if (io.DeltaTime > 0.0f) cpu_age_ += io.DeltaTime;
      if (cpu_age_ >= 1.0f || last_sys_tick_ == 0) {
        cpu_age_ = 0.0f;
        uint64_t now_tick;
        __asm__ __volatile__("mrs %x0, cntpct_el0" : "=r"(now_tick));
        const uint64_t cur_ticks = os::switch_cpu_profiler::TotalThreadTicks();
        if (last_sys_tick_ != 0 && now_tick > last_sys_tick_ && cur_ticks >= last_cpu_ticks_) {
          process_cpu_one_core_pct_ = double(cur_ticks - last_cpu_ticks_) * 100.0 / double(now_tick - last_sys_tick_);
        }
        last_sys_tick_ = now_tick;
        last_cpu_ticks_ = cur_ticks;
      }
      ImGui::Text("Process CPU (all threads): %.0f%% of one core, %.0f%% of the four",
                  process_cpu_one_core_pct_, process_cpu_one_core_pct_ / 4.0);
#endif
      ImGui::Separator();
      auto row = [](const char* label, const ImVec4& col, double cur_ms, double avg_ms) {
        ImGui::TextColored(col, "%-34s", label);
        ImGui::SameLine();
        ImGui::Text("%6.2f ms  (avg %6.2f ms)", cur_ms, avg_ms);
      };
      row("Game thread CPU (active)",   ImVec4(0.35f, 0.75f, 1.00f, 1.0f), p.cur_cpu_active_ms,  p.avg_cpu_active_ms);
      row("Game waiting for fences",    ImVec4(0.70f, 0.55f, 1.00f, 1.0f), p.cur_wait_fence_ms,  p.avg_wait_fence_ms);
      row("Game waiting for Swap (ahead)", ImVec4(1.00f, 0.65f, 0.25f, 1.0f), p.cur_wait_swap_ms, p.avg_wait_swap_ms);
      row("Game waiting for display",   ImVec4(0.65f, 0.65f, 0.65f, 1.0f), p.cur_wait_display_ms, p.avg_wait_display_ms);
      row("Game waiting for limiter",   ImVec4(0.55f, 0.55f, 0.55f, 1.0f), p.cur_wait_limiter_ms, p.avg_wait_limiter_ms);
      row("Render thread (Renut)",      ImVec4(1.00f, 0.90f, 0.30f, 1.0f), p.cur_render_ms,      p.avg_render_ms);
      row("GPU Frame",                  ImVec4(0.40f, 1.00f, 0.40f, 1.0f), p.cur_gpu_ms,         p.avg_gpu_ms);
      ImGui::Separator();
#if REX_PLATFORM_SWITCH
      ImGui::Text("Hitches (> 45 ms): %d   |   Stack profiler (perfil_pilas.flag): %s",
                  p.hitches_45ms, RexSwitchPerfStacksActive() ? "ON" : "off");
#else
      ImGui::Text("Hitches (> 45 ms): %d   |   Stack profiler (perfil_pilas.flag): n/a",
                  p.hitches_45ms);
#endif
    }
    ImGui::End();
    if (!pacing_open) rex::cvar::SetFlagByName("banjo_show_profiler", "false");
  }

 private:
  static constexpr size_t kSeriesCount = static_cast<size_t>(banjo::profiler::Series::kCount);
  static constexpr size_t kHistoryCount = 256;

  static ImVec4 Color(size_t s) {
    static const ImVec4 kColors[] = {
        ImVec4(0.95f, 0.95f, 0.95f, 1.0f),  // Application
        ImVec4(0.35f, 0.75f, 1.00f, 1.0f),  // Update
        ImVec4(1.00f, 0.65f, 0.25f, 1.0f),  // Draw
        ImVec4(0.55f, 0.55f, 0.55f, 1.0f),  // Limiter
        ImVec4(0.40f, 1.00f, 0.40f, 1.0f),  // GPU
        ImVec4(1.00f, 0.40f, 0.75f, 1.0f),  // Present
        ImVec4(1.00f, 0.90f, 0.30f, 1.0f),  // Render thread (Renut's renderer)
    };
    return kColors[s % (sizeof(kColors) / sizeof(kColors[0]))];
  }

  double Average(size_t s) const {
    double sum = 0.0;
    size_t n = 0;
    for (float v : history_[s]) {
      if (v > 0.0f) {
        sum += v;
        ++n;
      }
    }
    return n ? sum / double(n) : 0.0;
  }

  void DrawGraph() {
    ImGui::TextUnformatted("Frame Time (ms)");
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 160.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 220));

    // Unleashed fixes the axis at 0-20 ms for 60 FPS; Banjo runs at 30, so grow to fit (at least 40 ms).
    float top = 40.0f;
    for (size_t s = 0; s < kSeriesCount; ++s)
      for (float v : history_[s]) top = std::max(top, std::min(v, 250.0f));
    for (float ms : {16.67f, 33.33f, 50.0f, 100.0f, 200.0f}) {
      if (ms >= top) break;
      const float y = p1.y - (ms / top) * size.y;
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(80, 80, 90, 160));
      char label[16];
      std::snprintf(label, sizeof(label), "%.0f", ms);
      dl->AddText(ImVec2(p0.x + 2.0f, y - ImGui::GetTextLineHeight()), IM_COL32(150, 150, 160, 255), label);
    }

    for (size_t s = 0; s < kSeriesCount; ++s) {
      if (!banjo::profiler::HasValue(static_cast<banjo::profiler::Series>(s))) continue;
      const ImU32 col = ImGui::ColorConvertFloat4ToU32(Color(s));
      ImVec2 pts[kHistoryCount];
      for (size_t i = 0; i < kHistoryCount; ++i) {
        const float v = std::min(history_[s][(index_ + i) % kHistoryCount], top);
        pts[i] = ImVec2(p0.x + size.x * float(i) / float(kHistoryCount - 1), p1.y - (v / top) * size.y);
      }
      dl->AddPolyline(pts, int(kHistoryCount), col, 0, 1.5f);
    }
    ImGui::Dummy(size);
  }

  std::array<std::array<float, kHistoryCount>, kSeriesCount> history_{};
  size_t index_ = 0;
  std::string last_threads_;
  float threads_age_ = 0.0f;
  float cpu_age_ = 0.0f;
  float pacing_age_ = 1.0f;
  renut::frame_stats::PacingSnapshot last_pacing_{};
  uint64_t last_sys_tick_ = 0;
  uint64_t last_cpu_ticks_ = 0;
  double process_cpu_one_core_pct_ = 0.0;
};
