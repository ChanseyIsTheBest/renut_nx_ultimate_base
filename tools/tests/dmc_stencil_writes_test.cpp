// dmc_devil-nx (renut-ultimate's port of its GPU batch item #41): render/draw.cpp DmcStencilWrites against a field-by-field
// model of RB_DEPTHCONTROL (STENCIL_ENABLE bit 0, BACKFACE_ENABLE bit 7, the front face's fail / z-pass / z-fail operations
// at bits 11, 14 and 17, the back face's at 23, 26 and 29, 3 bits each, KEEP = 0) and the stencil write mask
// (StencilRefMask bits 16-23), with the key on and off (off: the base's test).
// Build: clang++ -std=c++20 -O2 tools/tests/dmc_stencil_writes_test.cpp (0 mismatches).
#include <cstdint>
#include <cstdio>
#include <random>
using u32 = uint32_t;
static bool g_exact = true;
// The helper as in render/draw.cpp, its static key read replaced by g_exact.
inline bool DmcStencilWrites(u32 depth_control, u32 stencil_ref_mask) {
  const bool exact = g_exact;
  const bool enabled_with_mask = (depth_control & 1) && ((stencil_ref_mask >> 16) & 0xFF) != 0;  // the base's test
  if (!exact || !enabled_with_mask)
    return enabled_with_mask;
  return ((depth_control >> 11) & 0x1FFu) != 0u || ((depth_control & 0x80u) && ((depth_control >> 23) & 0x1FFu) != 0u);
}
static bool Model(u32 dc, u32 rm, bool exact) {
  if (!(dc & 1) || ((rm >> 16) & 0xFF) == 0)
    return false;
  if (!exact)
    return true;
  bool changes = false;
  for (int f = 0; f < 3; ++f)
    changes |= ((dc >> (11 + 3 * f)) & 7u) != 0;  // the front face (both faces without BACKFACE_ENABLE)
  if (dc & 0x80u)
    for (int f = 0; f < 3; ++f)
      changes |= ((dc >> (23 + 3 * f)) & 7u) != 0;
  return changes;
}
int main() {
  std::mt19937 rng(41);
  int bad = 0;
  for (int t = 0; t < 2000000; ++t) {
    u32 dc = u32(rng()), rm = u32(rng());
    if (t & 1) dc &= ~(0x1FFu << 11);  // many all-KEEP front faces
    if (t & 2) dc &= ~(0x1FFu << 23);  // and back faces
    if (t & 4) rm &= ~(0xFFu << 16);   // and empty write masks
    for (int k = 0; k < 2; ++k) {
      g_exact = k != 0;
      bad += DmcStencilWrites(dc, rm) != Model(dc, rm, g_exact);
    }
  }
  std::printf("DmcStencilWrites vs the field model: %d mismatches in 4000000 cases\n", bad);
  return bad != 0;
}
