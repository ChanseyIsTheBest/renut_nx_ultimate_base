// Port 388 (ssasr-nx 941eac99, N18; ssasr_tags_narrow): the map parser (app/src/renut_engine/gpu/d3d/tags_narrow_map.h)
// and the narrowed TagChunks rule (d3d_native.h), against a model of the touch table: the fetch constants' lines
// 0x480-0x77F are bits 0-11, the tail from 0x2780 bits 12 on, nothing in between.
// Build: clang++ -std=c++20 -O2 -I app/src tools/tests/tags_narrow_map_test.cpp && ./a.out (exit 0: all pass)
#include "renut_engine/gpu/d3d/tags_narrow_map.h"

#include <bit>
#include <cstdio>

using renut::gpu::native::ParseTagsNarrowMap;
using renut::gpu::native::TagsNarrowMap;

namespace {
int g_fail = 0;
void Check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_fail;
  }
}

struct Layout {
  uint32_t snapshot;  // kDeviceSnapshotSize
  uint32_t lines() const { return (snapshot + 63) / 64; }
  uint64_t Bits(uint32_t offset, uint32_t size) const {  // d3d_native.h TouchBits
    uint64_t bits = 0;
    for (uint32_t line = offset >> 6; size && line < lines() && line * 64 < offset + size; ++line) {
      const uint32_t at = line * 64;
      if (at >= 0x480 && at < 0x780) bits |= uint64_t(1) << ((at - 0x480) / 64);
      else if (at >= 0x2780) bits |= uint64_t(1) << (12 + (at - 0x2780) / 64);
    }
    return bits;
  }
};

bool Parse(const Layout &l, const char *text, TagsNarrowMap *map, std::string *error) {
  return ParseTagsNarrowMap(
      text, [&](uint32_t o, uint32_t s) { return l.Bits(o, s); }, l.lines() * 64, map, error);
}

// d3d_native.h TagChunks with the map (state chunks S for any other tag; the fetch part left out: it is unchanged).
uint64_t TagChunks(bool on, const TagsNarrowMap &m, uint64_t S, uint64_t states, uint64_t fetch_hi, uint64_t misc) {
  if (on && (states & ~m.tags) == 0 && (misc | fetch_hi) == 0) {
    uint64_t c = 0;
    for (uint64_t b = states; b; b &= b - 1) c |= m.chunks[std::countr_zero(b)];
    return c;
  }
  return (states | misc | fetch_hi) ? S : 0;
}
}  // namespace

int main() {
  const Layout katamari{0x31A0}, ssasr{0x3258};
  const char *ssasr_map =
      "10=0x2900+0x80,0x2A80+0x40,0x2EC0+0x40;17=0x2880+0x100,0x2A00+0xC0,0x2EC0+0x80,0x3240+0x40";
  TagsNarrowMap m;
  std::string e;

  // Empty: no map, no error.
  Check(Parse(katamari, "", &m, &e) && m.tags == 0, "empty text is an empty map");
  Check(Parse(katamari, "  ;  ", &m, &e) && m.tags == 0, "blank entries are skipped");

  // ssasr-nx's own map on its layout: both tags, the sets ssasr-nx's constants give.
  Check(Parse(ssasr, ssasr_map, &m, &e), "ssasr-nx's map parses on its layout");
  Check(m.tags == ((uint64_t(1) << 10) | (uint64_t(1) << 17)), "tags 10 and 17");
  Check(m.chunks[10] == (ssasr.Bits(0x2900, 0x80) | ssasr.Bits(0x2A80, 0x40) | ssasr.Bits(0x2EC0, 0x40)),
        "tag 10's chunks");
  Check(m.chunks[17] == (ssasr.Bits(0x2880, 0x100) | ssasr.Bits(0x2A00, 0xC0) | ssasr.Bits(0x2EC0, 0x80) |
                         ssasr.Bits(0x3240, 0x40)),
        "tag 17's chunks");
  // The same map on Beautiful Katamari's layout: 0x3240 is past its touch table, so the whole map is rejected.
  Check(!Parse(katamari, ssasr_map, &m, &e) && m.tags == 0 && !e.empty(), "ssasr-nx's map rejected on Katamari's");

  // Faults reject the whole map.
  const char *bad[] = {"64=0x2780+0x40",          "10=0x2780+0x40;10=0x2800+0x40", "10=",
                       "10=0x2780",               "10=0x2780+0",                   "10=0x2780+0x40,zz+4",
                       "x=0x2780+0x40",           "10=0x1000+0x40",                "10=0x31A0+0x40",
                       "10=0x2780+0x40;17=0x800+4", "10=0x2780+0x40 trailing",      "10=0x123456789+4"};
  for (const char *t : bad) {
    const bool ok = Parse(katamari, t, &m, &e);
    if (ok || m.tags != 0 || e.empty()) {
      std::printf("FAIL: '%s' should be rejected\n", t);
      ++g_fail;
    }
  }
  Check(Parse(katamari, " 3 = 0x2780 + 0x40 , 10176+64 ", &m, &e) && m.tags == 8 &&
            m.chunks[3] == katamari.Bits(0x2780, 0x80),
        "spaces and decimal");

  // The rule: off, or a tag outside the map, or a misc / high fetch tag: all 17 chunks as before.
  const uint64_t S = katamari.Bits(0x2780, 0x300) | katamari.Bits(0x2DF4, 0x108);
  Check(Parse(katamari, "10=0x2900+0x80;17=0x2880+0x100", &m, &e), "a Katamari-layout map parses");
  const uint64_t t10 = uint64_t(1) << 10, t17 = uint64_t(1) << 17, t3 = uint64_t(1) << 3;
  Check(TagChunks(false, m, S, t10, 0, 0) == S, "off: tag 10 stands for all");
  Check(TagChunks(true, m, S, t10, 0, 0) == m.chunks[10], "on: tag 10 alone narrowed");
  Check(TagChunks(true, m, S, t10 | t17, 0, 0) == (m.chunks[10] | m.chunks[17]), "on: 10 and 17 both narrowed");
  Check(TagChunks(true, m, S, t10 | t3, 0, 0) == S, "on: a tag outside the map: all");
  Check(TagChunks(true, m, S, t10, 1, 0) == S, "on: a high fetch tag: all");
  Check(TagChunks(true, m, S, t10, 0, 1) == S, "on: a misc tag: all");
  Check(TagChunks(true, m, S, 0, 0, 0) == 0, "on: no tag: none");
  Check(TagChunks(true, TagsNarrowMap{}, S, t10, 0, 0) == S, "on, empty map: all");

  std::printf(g_fail ? "%d failed\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
