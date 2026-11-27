// Port 391 (2026-11-28, the user: "Yes, add the guard"): the float constants' layout guard for the delta chain
// (backend/queued.cpp CaptureDelta; renut_delta_layout_guard). No project headers, so
// tools/tests/delta_layout_guard_test.cpp builds it alone.
//
// Every delta path (the marked deltas, the staged deltas, the group copies, the original compare) takes a draw's float
// constants as the groups m_Pending words 0 and 1 mark dirty (bit 63 - group, four registers a group), copied from
// where d3d.h's Device puts them (m_Constants at 0x480: the vertex shader's 4 KB at 0x780, the pixel shader's at 0x1780).
// Both halves hold only when the game's XDK lays its device out the same way. A Gears of War port's did not: its float
// constants lay elsewhere, every delta path copied the wrong bytes, and the whole picture came out grey with black
// patches, with every verify passing (both sides of a verify read the same place).
//
// The check: between two looks at the device (every Nth capture of the chain's device), every group whose bytes
// changed must have been marked by one of the captures in between, or by a whole capture that consumed the marks
// (Fold: queued.cpp ClearPending; the chain restarts every frame, and a Begin*Vertices draw's whole capture after that
// clears the marks with no delta to give them to). The 360's own Direct3D uploads only the groups its pending bits
// name, so a game never writes a constant without marking it. A changed group nobody marked means the
// constants are not where d3d.h says (or a path lost a mark): Tripped. Separately, when the groups marked over the
// run's looks add up to kNeverChangesAfter and not one of them ever changed: NeverChanges, once (a warning: a game
// whose constants never change at all is possible only in principle).
//
// Its cost: at each capture a pointer compare, two ORs and a count; at a look (every Nth capture, 256 by default) an
// 8 KB compare and an 8 KB copy (tools/tests/delta_layout_guard_test.cpp times one). A layout fault shows at the first
// looks that see a constant change, so after retire_after clean looks that saw changes (1024 by default: about 262,000
// captures, the first minutes of a run) it stops looking for the rest of the run: Retired. Both keys can turn it off
// outright (renut_delta_layout_guard = 0) once a port's layout is known to be right.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace renut::gpu::layout_guard {

constexpr uint32_t kGroups = 64;                         // a stage's: 256 registers, four a group
constexpr uint32_t kGroupBytes = 64;                     // four registers of four floats
constexpr uint32_t kStageBytes = kGroups * kGroupBytes;  // 4 KB
constexpr uint32_t kRegionBytes = 2 * kStageBytes;       // the vertex shader's, then the pixel shader's
constexpr uint64_t kNeverChangesAfter = 16384;           // marked groups over the run's looks, none changed

// The groups of one stage whose bytes differ (bit 63 - group, as m_Pending).
inline uint64_t ChangedGroups(const uint8_t *now, const uint8_t *was) {
  uint64_t mask = 0;
  for (uint32_t g = 0; g < kGroups; ++g)
    if (std::memcmp(now + g * kGroupBytes, was + g * kGroupBytes, kGroupBytes) != 0)
      mask |= uint64_t(1) << (63 - g);
  return mask;
}

enum class Result { kNone, kTripped, kNeverChanges, kRetired };

struct Guard {
  uint32_t period = 0;           // looks every Nth capture; 0: off (or retired)
  uint32_t retire_after = 0;     // clean looks, with changes seen, before it stops looking; 0: never
  const void *device = nullptr;  // the device the window is on
  bool armed = false;            // a snapshot taken
  uint32_t since = 0;            // captures since the last look
  uint64_t vs = 0, ps = 0;       // the groups marked since the snapshot
  uint64_t marked = 0, changed = 0, looks = 0;  // over the run's looks
  bool tripped = false, warned = false, retired = false;
  uint64_t unmarked_vs = 0, unmarked_ps = 0;  // the trip's groups
  alignas(64) uint8_t snap[kRegionBytes];

  // At each capture of the chain's device, before its marks are consumed: region = its float constants where d3d.h
  // puts them (the vertex shader's 4 KB then the pixel shader's); vs, ps = the groups this capture's marks name.
  Result Note(const void *dev, const uint8_t *region, uint64_t vs_marks, uint64_t ps_marks) {
    if (period == 0 || tripped)
      return Result::kNone;
    if (dev != device || !armed) {  // a new window: what this capture's marks name is already in the snapshot
      device = dev;
      armed = true;
      std::memcpy(snap, region, kRegionBytes);
      vs = ps = 0;
      since = 0;
      return Result::kNone;
    }
    vs |= vs_marks;
    ps |= ps_marks;
    if (++since < period)
      return Result::kNone;
    since = 0;
    ++looks;
    const uint64_t cv = ChangedGroups(region, snap), cp = ChangedGroups(region + kStageBytes, snap + kStageBytes);
    marked += uint64_t(std::popcount(vs) + std::popcount(ps));
    changed += uint64_t(std::popcount(cv) + std::popcount(cp));
    const uint64_t uv = cv & ~vs, up = cp & ~ps;
    std::memcpy(snap, region, kRegionBytes);
    vs = ps = 0;
    if (uv | up) {
      tripped = true;
      unmarked_vs = uv;
      unmarked_ps = up;
      return Result::kTripped;
    }
    if (!warned && changed == 0 && marked >= kNeverChangesAfter) {
      warned = true;
      return Result::kNeverChanges;
    }
    // Retired only once marked groups have been seen changing where d3d.h says (a window with no change at all proves
    // nothing about the layout).
    if (retire_after != 0 && looks >= retire_after && changed != 0) {
      retired = true;
      period = 0;
      return Result::kRetired;
    }
    return Result::kNone;
  }
  // Marks another capture consumed (a whole capture's), on the window's device.
  void Fold(const void *dev, uint64_t vs_marks, uint64_t ps_marks) {
    if (period != 0 && armed && dev == device) {
      vs |= vs_marks;
      ps |= ps_marks;
    }
  }
  // The marks went somewhere the guard cannot see (a command buffer recorded on the window's device): a new window.
  void Restart(const void *dev) {
    if (dev == device)
      armed = false;
  }
};

}  // namespace renut::gpu::layout_guard
