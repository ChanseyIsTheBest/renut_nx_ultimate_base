// ssasr-nx (941eac99, N18; ssasr_tags_narrow, port 388): the per-port map of narrowed state tags, from the toml key
// ssasr_tags_narrow_map (banjo_renut_tuning.cpp). No project headers, so tools/tests/tags_narrow_map_test.cpp builds it
// alone.
//
// The form: "TAG=OFFSET+SIZE,OFFSET+SIZE;TAG=..." with TAG a bit of m_Pending word 2 (0-63) and each OFFSET+SIZE a
// device byte range (hex with 0x, or decimal); spaces are ignored. ssasr-nx's, for its XDK 2.0.9328 device (not the
// base's: its private render state sits 0xB8 further on):
//   "10=0x2900+0x80,0x2A80+0x40,0x2EC0+0x40;17=0x2880+0x100,0x2A00+0xC0,0x2EC0+0x80,0x3240+0x40"
// Each tag's chunks are the touch bits of its ranges. A range must end within the touch table (d3d_native.h kTouchLines
// lines of 64 bytes) and hit at least one chunk (the untracked bytes between the fetch constants and 0x2780 hold none).
// A set may name chunks outside the 17 render-state chunks (ssasr-nx's tag 2.17 writer stores 0x3244 too): an extra
// chunk is only one more compare. Any fault (a bad number, a tag twice, a tag above 63, an empty range or one outside)
// rejects the whole map: false, the error says which, the map stays empty.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace renut::gpu::native {

struct TagsNarrowMap {
  uint64_t tags = 0;         // the tags with a narrowed set
  uint64_t chunks[64] = {};  // per tag: its chunks
};

namespace tags_narrow_detail {
inline std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
// A number of at most 0xFFFFFFFF, hex with 0x or decimal, nothing else in it.
inline bool Number(std::string_view s, uint64_t *out) {
  s = Trim(s);
  uint64_t base = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s.remove_prefix(2);
  }
  if (s.empty() || s.size() > 10) return false;
  uint64_t v = 0;
  for (char c : s) {
    uint64_t d;
    if (c >= '0' && c <= '9') d = uint64_t(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f') d = uint64_t(c - 'a' + 10);
    else if (base == 16 && c >= 'A' && c <= 'F') d = uint64_t(c - 'A' + 10);
    else return false;
    v = v * base + d;
  }
  if (v > 0xFFFFFFFFull) return false;
  *out = v;
  return true;
}
}  // namespace tags_narrow_detail

// touch_bits(offset, size): the chunk bits of a device byte range (d3d_native.h TouchBits); limit: the touch table's
// end in bytes (kTouchLines * 64).
template <class TouchBitsFn>
bool ParseTagsNarrowMap(std::string_view text, TouchBitsFn touch_bits, uint32_t limit, TagsNarrowMap *out,
                        std::string *error) {
  using tags_narrow_detail::Number;
  using tags_narrow_detail::Trim;
  TagsNarrowMap map;
  auto fail = [&](std::string why) {
    *out = TagsNarrowMap{};
    *error = std::move(why);
    return false;
  };
  text = Trim(text);
  while (!text.empty()) {
    const size_t semi = text.find(';');
    const std::string_view entry = Trim(text.substr(0, semi));
    text = semi == std::string_view::npos ? std::string_view() : text.substr(semi + 1);
    if (entry.empty()) continue;
    const size_t eq = entry.find('=');
    uint64_t tag = 0;
    if (eq == std::string_view::npos || !Number(entry.substr(0, eq), &tag))
      return fail("an entry without TAG=: '" + std::string(entry) + "'");
    if (tag > 63) return fail("tag " + std::to_string(tag) + " is above 63");
    if (map.tags & (uint64_t(1) << tag)) return fail("tag " + std::to_string(tag) + " named twice");
    uint64_t chunks = 0;
    std::string_view ranges = entry.substr(eq + 1);
    if (Trim(ranges).empty()) return fail("tag " + std::to_string(tag) + " has no range");
    while (!ranges.empty()) {
      const size_t comma = ranges.find(',');
      const std::string_view range = Trim(ranges.substr(0, comma));
      ranges = comma == std::string_view::npos ? std::string_view() : ranges.substr(comma + 1);
      const size_t plus = range.find('+');
      uint64_t offset = 0, size = 0;
      if (plus == std::string_view::npos || !Number(range.substr(0, plus), &offset) ||
          !Number(range.substr(plus + 1), &size))
        return fail("tag " + std::to_string(tag) + ": not OFFSET+SIZE: '" + std::string(range) + "'");
      if (size == 0 || offset + size > limit)
        return fail("tag " + std::to_string(tag) + ": range '" + std::string(range) + "' is empty or past the device");
      const uint64_t bits = touch_bits(uint32_t(offset), uint32_t(size));
      if (bits == 0)
        return fail("tag " + std::to_string(tag) + ": range '" + std::string(range) + "' holds no chunk");
      chunks |= bits;
    }
    map.tags |= uint64_t(1) << tag;
    map.chunks[tag] = chunks;
  }
  *out = map;
  error->clear();
  return true;
}

}  // namespace renut::gpu::native
