// Port 391 (2026-11-28): the delta chain's float constant layout guard (app/src/renut_engine/gpu/backend/
// delta_layout_guard.h) against a model: a device whose float constants lie at a true offset, the game's constant writes
// marking their group (bit 63 - group, as m_Pending), and the guard reading where d3d.h says (0x780). Also times a look.
// Build: clang++ -std=c++20 -O2 -I app/src tools/tests/delta_layout_guard_test.cpp && ./a.out (exit 0: all pass)
#include "renut_engine/gpu/backend/delta_layout_guard.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace lg = renut::gpu::layout_guard;

namespace {
int g_fail = 0;
void Check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_fail;
  }
}

constexpr uint32_t kAssumed = 0x780;  // d3d.h: m_Constants 0x480 + VertexShaderF 0x300

struct Device {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x8000, 0);
  uint32_t constants;          // where this game's XDK keeps the float constants
  uint64_t pending[2] = {0, 0};
  explicit Device(uint32_t at) : constants(at) {}
  // SetVertexShaderConstantF / SetPixelShaderConstantF: 4 floats into a register, its group marked.
  void SetConstant(int stage, uint32_t reg, uint32_t value) {
    std::memcpy(bytes.data() + constants + stage * lg::kStageBytes + reg * 16, &value, 4);
    pending[stage] |= uint64_t(1) << (63 - reg / 4);
  }
};

struct Run {
  lg::Result first = lg::Result::kNone;
  uint64_t captures_to_first = 0;
  bool retired = false, warned = false, tripped = false;
};

// Draws: each sets a few constants (some again with the value they had), then a capture: Note, the marks consumed.
Run Drive(Device &d, lg::Guard &g, uint64_t draws, uint32_t seed, bool unmarked_noise_at_assumed = false,
          bool same_values = false) {
  std::mt19937 rng(seed);
  Run r;
  for (uint64_t n = 0; n < draws; ++n) {
    const int sets = int(rng() % 6);
    for (int k = 0; k < sets; ++k) {
      const int stage = int(rng() % 2);
      const uint32_t reg = rng() % 256;
      d.SetConstant(stage, reg, same_values ? 0x3F800000u : rng());
    }
    if (unmarked_noise_at_assumed && rng() % 50 == 0) {  // other device fields at the assumed place change, unmarked
      const uint32_t at = kAssumed + (rng() % lg::kRegionBytes);
      d.bytes[at] ^= 0x5A;
    }
    const lg::Result res = g.Note(&d, d.bytes.data() + kAssumed, d.pending[0], d.pending[1]);
    d.pending[0] = d.pending[1] = 0;
    if (res != lg::Result::kNone && r.first == lg::Result::kNone) {
      r.first = res;
      r.captures_to_first = n + 1;
    }
    r.retired |= res == lg::Result::kRetired;
    r.warned |= res == lg::Result::kNeverChanges;
    r.tripped |= res == lg::Result::kTripped;
  }
  return r;
}

lg::Guard *NewGuard(uint32_t period, uint32_t retire) {
  auto *g = new lg::Guard();
  g->period = period;
  g->retire_after = retire;
  return g;
}
}  // namespace

int main() {
  {  // The layout matches: never trips over 2 M captures with every look kept (no retiring).
    Device d(kAssumed);
    lg::Guard *g = NewGuard(256, 0);
    const Run r = Drive(d, *g, 2000000, 1);
    Check(!r.tripped && !r.warned, "matching layout: no trip, no warning");
    Check(g->changed > 0 && g->looks == 2000000 / 256 - 0, "matching layout: looks made, changes seen");
    delete g;
  }
  {  // The layout matches, retiring after 1024 clean looks.
    Device d(kAssumed);
    lg::Guard *g = NewGuard(256, 1024);
    const Run r = Drive(d, *g, 400000, 2);
    Check(r.retired && !r.tripped && g->period == 0, "matching layout: retired after 1024 looks");
    Check(r.captures_to_first == 1 + 1024ull * 256, "retired at the 1024th look");
    delete g;
  }
  {  // Constants set again with the same value (marked, unchanged): no trip.
    Device d(kAssumed);
    lg::Guard *g = NewGuard(1, 0);
    const Run r = Drive(d, *g, 20000, 3, false, true);
    Check(!r.tripped, "same values set again: no trip");
    delete g;
  }
  {  // The constants 0x80 further on than d3d.h says (two groups): trips at an early look.
    Device d(kAssumed + 0x80);
    lg::Guard *g = NewGuard(256, 1024);
    const Run r = Drive(d, *g, 400000, 4);
    Check(r.tripped && r.first == lg::Result::kTripped, "shifted layout: tripped");
    Check(r.captures_to_first <= 4 * 256 + 1, "shifted layout: within the first looks");
    Check(!r.retired, "shifted layout: never retired");
    delete g;
  }
  {  // The constants elsewhere, the assumed place never changing: no trip, the warning once.
    Device d(0x2800);
    lg::Guard *g = NewGuard(256, 1024);
    const Run r = Drive(d, *g, 400000, 5);
    Check(!r.tripped && r.warned && !r.retired, "constants elsewhere, assumed place still: warned, not retired");
    delete g;
  }
  {  // The constants elsewhere, other fields at the assumed place changing unmarked: trips.
    Device d(0x2800);
    lg::Guard *g = NewGuard(256, 1024);
    const Run r = Drive(d, *g, 400000, 6, true);
    Check(r.tripped, "constants elsewhere, unmarked changes at the assumed place: tripped");
    delete g;
  }
  {  // Whole captures between the deltas (ClearPending: a Begin*Vertices draw after the chain's frame reset) consume
     // marks: folded, no trip; not folded (the dense PC run's false positive before the fold), a trip.
    for (const bool fold : {true, false}) {
      Device d(kAssumed);
      lg::Guard *g = NewGuard(16, 0);
      std::mt19937 rng(10);
      bool tripped = false;
      for (int n = 0; n < 200000; ++n) {
        d.SetConstant(int(rng() % 2), rng() % 256, rng());
        if (rng() % 5 == 0) {  // a whole capture
          if (fold)
            g->Fold(&d, d.pending[0], d.pending[1]);
        } else {
          tripped |= g->Note(&d, d.bytes.data() + kAssumed, d.pending[0], d.pending[1]) == lg::Result::kTripped;
        }
        d.pending[0] = d.pending[1] = 0;
      }
      Check(fold ? !tripped : tripped, fold ? "whole captures folded: no trip" : "whole captures not folded: a trip");
      delete g;
    }
  }
  {  // A command buffer recorded on the window's device: a new window, no trip.
    Device d(kAssumed);
    lg::Guard *g = NewGuard(16, 0);
    std::mt19937 rng(11);
    bool tripped = false;
    for (int n = 0; n < 100000; ++n) {
      d.SetConstant(int(rng() % 2), rng() % 256, rng());
      if (rng() % 50 == 0) {
        g->Restart(&d);  // the marks went to the buffer
      } else {
        tripped |= g->Note(&d, d.bytes.data() + kAssumed, d.pending[0], d.pending[1]) == lg::Result::kTripped;
      }
      d.pending[0] = d.pending[1] = 0;
    }
    Check(!tripped, "recorded on the window's device: restarted, no trip");
    delete g;
  }
  {  // A device switch starts a new window: B's writes (consumed by B's captures) never trip A's.
    Device a(kAssumed), b(kAssumed);
    lg::Guard *g = NewGuard(8, 0);
    std::mt19937 rng(7);
    bool tripped = false;
    for (int n = 0; n < 200000; ++n) {
      Device &d = (n / 37) % 2 ? b : a;
      d.SetConstant(int(rng() % 2), rng() % 256, rng());
      tripped |= g->Note(&d, d.bytes.data() + kAssumed, d.pending[0], d.pending[1]) == lg::Result::kTripped;
      d.pending[0] = d.pending[1] = 0;
    }
    Check(!tripped, "two devices: no trip");
    delete g;
  }
  {  // Off: nothing, whatever the layout.
    Device d(kAssumed + 0x80);
    lg::Guard *g = NewGuard(0, 1024);
    const Run r = Drive(d, *g, 100000, 8);
    Check(r.first == lg::Result::kNone && g->looks == 0, "off: no look");
    delete g;
  }
  {  // The cost of a look: the 8 KB compare and copy.
    Device d(kAssumed);
    lg::Guard *g = NewGuard(1, 0);
    std::mt19937 rng(9);
    for (int k = 0; k < 64; ++k)
      d.SetConstant(int(rng() % 2), rng() % 256, rng());
    g->Note(&d, d.bytes.data() + kAssumed, 0, 0);
    const int n = 200000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < n; ++k) {
      d.bytes[kAssumed + (k % lg::kRegionBytes)] ^= 1;  // one change a look, marked
      g->Note(&d, d.bytes.data() + kAssumed, ~uint64_t(0), ~uint64_t(0));
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("a look: %.2f us here (every 256th capture by default: %.4f us a capture)\n", us, us / 256);
    delete g;
  }
  std::printf(g_fail ? "%d failed\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
