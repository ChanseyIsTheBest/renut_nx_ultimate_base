// dmc_devil-nx (patch 20, dmc_record_index_pending_count): RecordDeferredDraw's per-device pending counts
// (app/src/renut_engine/gpu/guest/command_buffers.cpp) against FlushPending, which takes one count off per entry, as a
// model of the three places: a recorded BeginIndexedVertices draw pushes two entries (its vertices, its indices); the
// hooks' first step (FlushRecordedVertices, blur_renut_flush_fast_local) copies nothing while the device's count is 0.
// Build: clang++ -std=c++20 -O2 tools/tests/record_pending_count_test.cpp && ./a.out (exit 0: with the fix the counts
// stay balanced and every entry is copied before the scratch is written again; the base's form shows the miss).
#include <cstdint>
#include <cstdio>
#include <vector>

struct Model {
  bool count_each;
  std::vector<int> pending;  // g_pending_vertices of one device: the draw each entry belongs to
  uint32_t count = 0;        // g_recording_devices[device].pending and PendingOf(device)
  std::vector<int> copied;   // CopyPending, in order
  void Record(int draw, bool indices) {  // RecordDeferredDraw
    pending.push_back(draw);
    if (indices)
      pending.push_back(draw);
    count += 1;
    if (count_each && indices)
      count += 1;
  }
  void HookFirstStep() {  // FlushRecordedVertices: the fast path, then FlushDevicePending
    if (count == 0)
      return;
    for (int draw : pending) {
      copied.push_back(draw);
      count -= 1;
    }
    pending.clear();
  }
};

// BeginIndexedVertices (draw 1), its data written, the next hook; BeginVertices (draw 2), its data written, the next
// hook; then BeginVertices (draw 3), whose call is where the game may write over the scratch.
static bool Run(bool count_each, bool verbose) {
  Model m{count_each};
  m.Record(1, true);
  m.HookFirstStep();
  const uint32_t after_indexed = m.count;
  m.Record(2, false);
  m.HookFirstStep();
  const bool draw2_copied_in_time = m.pending.empty() && !m.copied.empty() && m.copied.back() == 2;
  if (verbose)
    std::printf("%s: count after the indexed draw's flush %08X, draw 2 copied before draw 3: %s\n",
                count_each ? "fixed" : "base ", after_indexed, draw2_copied_in_time ? "yes" : "no");
  return after_indexed == 0 && draw2_copied_in_time && m.count == 0;
}

int main() {
  const bool fixed = Run(true, true);
  const bool base = Run(false, true);
  // The fix balances; the base's single count wraps and misses draw 2 (the bug this key fixes).
  return (fixed && !base) ? 0 : 1;
}
