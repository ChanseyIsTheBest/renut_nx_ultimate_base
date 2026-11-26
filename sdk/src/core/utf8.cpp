/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <locale>
#include <numeric>
#include <tuple>
#include <utility>

#include <rex/logging.h>
#include <rex/string/utf8.h>

using citer = std::string_view::const_iterator;

namespace {

inline bool is_utf8_cont(uint8_t b) noexcept {
  return (b & 0xC0u) == 0x80u;
}

// Decodes one UTF-8 sequence at [p, end). Never throws: invalid or truncated
// bytes yield PEP-383 surrogate-escaped 0xDC00 | byte with length 1.
inline std::pair<uint32_t, std::ptrdiff_t> decode_utf8_at(citer p, citer end) noexcept {
  if (p >= end) {
    return {0u, 0};
  }
  const uint8_t b0 = static_cast<uint8_t>(*p);
  if (b0 <= 0x7Fu) {
    return {uint32_t(b0), 1};
  }
  const std::ptrdiff_t avail = end - p;
  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (avail >= 2) {
      const uint8_t b1 = static_cast<uint8_t>(p[1]);
      if (is_utf8_cont(b1)) {
        const uint32_t cp = ((uint32_t(b0) & 0x1Fu) << 6) | (uint32_t(b1) & 0x3Fu);
        return {cp, 2};
      }
    }
  } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (avail >= 3) {
      const uint8_t b1 = static_cast<uint8_t>(p[1]);
      const uint8_t b2 = static_cast<uint8_t>(p[2]);
      if (is_utf8_cont(b1) && is_utf8_cont(b2)) {
        const uint32_t cp = ((uint32_t(b0) & 0x0Fu) << 12) | ((uint32_t(b1) & 0x3Fu) << 6) |
                            (uint32_t(b2) & 0x3Fu);
        if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
          return {cp, 3};
        }
      }
    }
  } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (avail >= 4) {
      const uint8_t b1 = static_cast<uint8_t>(p[1]);
      const uint8_t b2 = static_cast<uint8_t>(p[2]);
      const uint8_t b3 = static_cast<uint8_t>(p[3]);
      if (is_utf8_cont(b1) && is_utf8_cont(b2) && is_utf8_cont(b3)) {
        const uint32_t cp = ((uint32_t(b0) & 0x07u) << 18) | ((uint32_t(b1) & 0x3Fu) << 12) |
                            ((uint32_t(b2) & 0x3Fu) << 6) | (uint32_t(b3) & 0x3Fu);
        if (cp >= 0x10000u && cp <= 0x10FFFFu) {
          return {cp, 4};
        }
      }
    }
  }
  return {0xDC00u | uint32_t(b0), 1};
}

class SafeUtf8Iter {
 public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = uint32_t;
  using difference_type = std::ptrdiff_t;
  using pointer = const uint32_t*;
  using reference = uint32_t;

  SafeUtf8Iter() = default;
  SafeUtf8Iter(citer it, citer begin, citer end) noexcept : it_(it), begin_(begin), end_(end) {}

  citer base() const noexcept { return it_; }

  uint32_t operator*() const noexcept { return decode_utf8_at(it_, end_).first; }

  SafeUtf8Iter& operator++() noexcept {
    if (it_ < end_) {
      it_ += decode_utf8_at(it_, end_).second;
    }
    return *this;
  }

  SafeUtf8Iter operator++(int) noexcept {
    SafeUtf8Iter tmp(*this);
    ++(*this);
    return tmp;
  }

  SafeUtf8Iter& operator--() noexcept {
    if (it_ <= begin_) {
      return *this;
    }
    const std::ptrdiff_t max_back = std::min<std::ptrdiff_t>(it_ - begin_, 4);
    for (std::ptrdiff_t len = max_back; len >= 2; --len) {
      if (decode_utf8_at(it_ - len, it_).second == len) {
        it_ -= len;
        return *this;
      }
    }
    --it_;
    return *this;
  }

  SafeUtf8Iter operator--(int) noexcept {
    SafeUtf8Iter tmp(*this);
    --(*this);
    return tmp;
  }

  bool operator==(const SafeUtf8Iter& other) const noexcept { return it_ == other.it_; }
  bool operator!=(const SafeUtf8Iter& other) const noexcept { return it_ != other.it_; }

 private:
  citer it_{};
  citer begin_{};
  citer end_{};
};

void append_safe(uint32_t cp, std::string& out) {
  if (cp >= 0xDC80u && cp <= 0xDCFFu) {
    out.push_back(static_cast<char>(cp & 0xFFu));
  } else if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.append("\xEF\xBF\xBD", 3);
  }
}

}  // namespace

using utf8_citer = SafeUtf8Iter;

namespace rex::string {

// Guest strings are not guaranteed to be well-formed (unpaired surrogates,
// invalid UTF-8 bytes, unterminated buffers), and Xbox/Windows APIs tolerate
// that. Replace malformed code units with U+FFFD (or preserve raw bytes on
// UTF-8 helpers) instead of ever throwing an unhandled exception.
std::string to_utf8(const std::u16string_view source) {
  std::string result;
  result.reserve(source.size());
  bool saw_invalid = false;
  for (size_t i = 0; i < source.size(); ++i) {
    uint32_t c = source[i];
    if (c >= 0xD800u && c <= 0xDBFFu && i + 1 < source.size() && source[i + 1] >= 0xDC00u &&
        source[i + 1] <= 0xDFFFu) {
      c = 0x10000u + ((c - 0xD800u) << 10) + (uint32_t(source[i + 1]) - 0xDC00u);
      ++i;
    } else if (c >= 0xD800u && c <= 0xDFFFu) {
      c = 0xFFFDu;
      saw_invalid = true;
    }
    append_safe(c, result);
  }
  if (saw_invalid) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("to_utf8: replaced invalid UTF-16 in guest string (first unit {:04X}, length {})",
                  source.empty() ? 0u : uint32_t(source[0]), source.size());
    }
  }
  return result;
}

std::u16string to_utf16(const std::string_view source) {
  std::u16string result;
  result.reserve(source.size());
  bool saw_invalid = false;
  citer p = source.cbegin();
  citer end = source.cend();
  while (p < end) {
    auto [cp, len] = decode_utf8_at(p, end);
    p += len;
    if (cp >= 0xDC80u && cp <= 0xDCFFu) {
      result.push_back(u'\uFFFD');
      saw_invalid = true;
    } else if (cp <= 0xFFFFu) {
      result.push_back(static_cast<char16_t>(cp));
    } else if (cp <= 0x10FFFFu) {
      const uint32_t v = cp - 0x10000u;
      result.push_back(static_cast<char16_t>(0xD800u + (v >> 10)));
      result.push_back(static_cast<char16_t>(0xDC00u + (v & 0x3FFu)));
    } else {
      result.push_back(u'\uFFFD');
      saw_invalid = true;
    }
  }
  if (saw_invalid) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("to_utf16: replaced invalid UTF-8 in string (length {})", source.size());
    }
  }
  return result;
}

std::string to_string(char32_t c) {
  std::string result;
  append_safe(static_cast<uint32_t>(c), result);
  return result;
}

uint32_t lower_ascii(const uint32_t c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

uint32_t upper_ascii(const uint32_t c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool equal_ascii_case(const uint32_t l, const uint32_t r) {
  return l == r || lower_ascii(l) == lower_ascii(r);
}

std::pair<utf8_citer, utf8_citer> make_citer(const std::string_view view) {
  return {utf8_citer(view.cbegin(), view.cbegin(), view.cend()),
          utf8_citer(view.cend(), view.cbegin(), view.cend())};
}

std::pair<utf8_citer, utf8_citer> make_citer(const utf8_citer begin, const utf8_citer end) {
  return {utf8_citer(begin.base(), begin.base(), end.base()),
          utf8_citer(end.base(), begin.base(), end.base())};
}

size_t byte_length(utf8_citer begin, utf8_citer end) {
  return size_t(std::distance(begin.base(), end.base()));
}

size_t utf8_count(const std::string_view view) {
  auto [begin, end] = make_citer(view);
  return static_cast<size_t>(std::distance(begin, end));
}

std::string utf8_lower_ascii(const std::string_view view) {
  auto [begin, end] = make_citer(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    append_safe(lower_ascii(*it), result);
  }
  return result;
}

std::string utf8_upper_ascii(const std::string_view view) {
  auto [begin, end] = make_citer(view);
  std::string result;
  for (auto it = begin; it != end; ++it) {
    append_safe(upper_ascii(*it), result);
  }
  return result;
}

template <bool LOWER>
inline size_t hash_fnv1a(const std::string_view view) {
  const size_t offset_basis = 0xCBF29CE484222325ull;
  const size_t prime = 0x00000100000001B3ull;
  auto work = [&prime](size_t hash, uint8_t byte_of_data) {
    hash ^= byte_of_data;
    hash *= prime;
    return hash;
  };
  auto hash = offset_basis;
  auto [begin, end] = make_citer(view);
  for (auto it = begin; it != end; ++it) {
    uint32_t c;
    if constexpr (LOWER) {
      c = lower_ascii(*it);
    } else {
      c = *it;
    }
    hash = work(hash, uint8_t((c >> 0) & 0xFF));
    hash = work(hash, uint8_t((c >> 8) & 0xFF));
    hash = work(hash, uint8_t((c >> 16) & 0xFF));
    hash = work(hash, uint8_t((c >> 24) & 0xFF));
  }
  return hash;
}

size_t utf8_hash_fnv1a(const std::string_view view) {
  return hash_fnv1a<false>(view);
}

size_t utf8_hash_fnv1a_case(const std::string_view view) {
  return hash_fnv1a<true>(view);
}

// TODO(gibbed): this is a separate inline function instead of inline within
// split due to a Clang bug: reference to local binding 'needle_begin' declared
// in enclosing function 'split'.
inline utf8_citer find_needle(utf8_citer haystack_it, utf8_citer haystack_end,
                              utf8_citer needle_begin, utf8_citer needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (c == *needle) {
        return true;
      }
    }
    return false;
  });
}

inline utf8_citer find_needle_case(utf8_citer haystack_it, utf8_citer haystack_end,
                                   utf8_citer needle_begin, utf8_citer needle_end) {
  return std::find_if(haystack_it, haystack_end, [&](const auto& c) {
    for (auto needle = needle_begin; needle != needle_end; ++needle) {
      if (equal_ascii_case(c, *needle)) {
        return true;
      }
    }
    return false;
  });
}

std::vector<std::string_view> utf8_split(const std::string_view haystack,
                                         const std::string_view needles, bool remove_empty) {
  std::vector<std::string_view> result;

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needles);

  auto it = haystack_begin;
  auto last = it;
  for (;;) {
    it = find_needle(it, haystack_end, needle_begin, needle_end);
    if (it == haystack_end) {
      break;
    }

    if (it != last) {
      auto offset = byte_length(haystack_begin, last);
      auto length = byte_length(haystack_begin, it) - offset;
      result.push_back(haystack.substr(offset, length));
    } else if (!remove_empty) {
      result.push_back("");
    }

    ++it;
    last = it;
  }

  if (last != haystack_end) {
    auto offset = byte_length(haystack_begin, last);
    result.push_back(haystack.substr(offset));
  }

  return result;
}

bool utf8_equal_z(const std::string_view left, const std::string_view right) {
  if (!left.size()) {
    return !right.size();
  } else if (!right.size()) {
    return false;
  }
  auto [left_begin, left_end] = make_citer(left);
  auto [right_begin, right_end] = make_citer(right);
  auto left_it = left_begin;
  auto right_it = right_begin;
  for (; left_it != left_end && *left_it != 0 && right_it != right_end && *right_it != 0;
       ++left_it, ++right_it) {
    if (*left_it != *right_it) {
      return false;
    }
  }
  return (left_it == left_end || *left_it == 0) && (right_it == right_end || *right_it == 0);
}

bool utf8_equal_case(const std::string_view left, const std::string_view right) {
  if (!left.size()) {
    return !right.size();
  } else if (!right.size()) {
    return false;
  }
  auto [left_begin, left_end] = make_citer(left);
  auto [right_begin, right_end] = make_citer(right);
  return std::equal(left_begin, left_end, right_begin, right_end, equal_ascii_case);
}

bool utf8_equal_case_z(const std::string_view left, const std::string_view right) {
  if (!left.size()) {
    return !right.size();
  } else if (!right.size()) {
    return false;
  }
  auto [left_begin, left_end] = make_citer(left);
  auto [right_begin, right_end] = make_citer(right);
  auto left_it = left_begin;
  auto right_it = right_begin;
  for (; left_it != left_end && *left_it != 0 && right_it != right_end && *right_it != 0;
       ++left_it, ++right_it) {
    if (!equal_ascii_case(*left_it, *right_it)) {
      return false;
    }
  }
  return (left_it == left_end || *left_it == 0) && (right_it == right_end || *right_it == 0);
}

std::string_view::size_type utf8_find_any_of(const std::string_view haystack,
                                             const std::string_view needles) {
  if (needles.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needles);

  auto it = find_needle(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return std::string_view::size_type(byte_length(haystack_begin, it));
}

std::string_view::size_type utf8_find_any_of_case(const std::string_view haystack,
                                                  const std::string_view needles) {
  if (needles.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needles);

  auto it = find_needle_case(haystack_begin, haystack_end, needle_begin, needle_end);
  if (it == haystack_end) {
    return std::string_view::npos;
  }

  return std::string_view::size_type(byte_length(haystack_begin, it));
}

std::string_view::size_type utf8_find_first_of(const std::string_view haystack,
                                               const std::string_view needle) {
  if (needle.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  for (; it != haystack_end; ++it) {
    it = std::find(it, haystack_end, *needle_begin);
    if (it == haystack_end) {
      return std::string_view::npos;
    }

    auto end = it;
    for (size_t i = 0; i < needle_count; ++i) {
      if (end == haystack_end) {
        // not enough room in target for search
        return std::string_view::npos;
      }
      ++end;
    }

    auto [sub_start, sub_end] = make_citer(it, end);
    if (std::equal(needle_begin, needle_end, sub_start, sub_end)) {
      return std::string_view::size_type(byte_length(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

std::string_view::size_type utf8_find_first_of_case(const std::string_view haystack,
                                                    const std::string_view needle) {
  if (needle.empty()) {
    return std::string_view::size_type(0);
  } else if (haystack.empty()) {
    return std::string_view::npos;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);
  auto nc = *needle_begin;

  auto it = haystack_begin;
  for (; it != haystack_end; ++it) {
    it = std::find_if(it, haystack_end,
                      [&nc](const uint32_t& c) { return equal_ascii_case(nc, c); });
    if (it == haystack_end) {
      return std::string_view::npos;
    }

    auto end = it;
    for (size_t i = 0; i < needle_count; ++i) {
      if (end == haystack_end) {
        // not enough room in target for search
        return std::string_view::npos;
      }
      ++end;
    }

    auto [sub_start, sub_end] = make_citer(it, end);
    if (std::equal(needle_begin, needle_end, sub_start, sub_end, equal_ascii_case)) {
      return std::string_view::size_type(byte_length(haystack_begin, it));
    }
  }

  return std::string_view::npos;
}

bool utf8_starts_with(const std::string_view haystack, char32_t needle) {
  if (haystack.empty()) {
    return false;
  }
  auto [it, end] = make_citer(haystack);
  return *it == uint32_t(needle);
}

bool utf8_starts_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      // not enough room in target for search
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = make_citer(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_starts_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_begin;
  auto end = it;
  for (size_t i = 0; i < needle_count; ++i) {
    if (end == haystack_end) {
      // not enough room in target for search
      return false;
    }
    ++end;
  }

  auto [sub_start, sub_end] = make_citer(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end, equal_ascii_case);
}

bool utf8_ends_with(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_end;
  auto end = it;
  --it;

  for (size_t i = 1; i < needle_count; ++i) {
    if (it == haystack_begin) {
      // not enough room in target for search
      return false;
    }
    --it;
  }

  auto [sub_start, sub_end] = make_citer(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end);
}

bool utf8_ends_with_case(const std::string_view haystack, const std::string_view needle) {
  if (needle.empty()) {
    return true;
  } else if (haystack.empty()) {
    return false;
  }

  auto [haystack_begin, haystack_end] = make_citer(haystack);
  auto [needle_begin, needle_end] = make_citer(needle);
  auto needle_count = utf8_count(needle);

  auto it = haystack_end;
  auto end = it;
  --it;

  for (size_t i = 0; i < needle_count; ++i) {
    if (it == haystack_begin) {
      // not enough room in target for search
      return false;
    }
    --it;
  }

  auto [sub_start, sub_end] = make_citer(it, end);
  return std::equal(needle_begin, needle_end, sub_start, sub_end, equal_ascii_case);
}

std::vector<std::string_view> utf8_split_path(const std::string_view path) {
  return utf8_split(path, "\\/", true);
}

std::string utf8_join_paths(const std::string_view left_path, const std::string_view right_path,
                            char32_t separator) {
  if (!left_path.length()) {
    return std::string(right_path);
  } else if (!right_path.length()) {
    return std::string(left_path);
  }

  utf8_citer it;
  std::tie(std::ignore, it) = make_citer(left_path);
  --it;

  std::string result = std::string(left_path);
  if (*it != static_cast<uint32_t>(separator)) {
    append_safe(static_cast<uint32_t>(separator), result);
  }
  return result + std::string(right_path);
}

std::string utf8_join_paths(const std::vector<std::string>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_join_paths(const std::vector<std::string_view>& paths, char32_t separator) {
  std::string result;
  auto it = paths.cbegin();
  if (it != paths.cend()) {
    result = *it++;
    for (; it != paths.cend(); ++it) {
      result = utf8_join_paths(result, *it, separator);
    }
  }
  return result;
}

std::string utf8_fix_path_separators(const std::string_view path, char32_t new_separator) {
  if (path.empty()) {
    return std::string();
  }

  // Swap all separators to new_sep.
  const char32_t old_separator = new_separator == U'\\' ? U'/' : U'\\';

  auto [path_begin, path_end] = make_citer(path);

  std::string result;
  auto it = path_begin;
  auto last = it;

  auto is_separator = [old_separator, new_separator](char32_t c) {
    return c == uint32_t(old_separator) || c == uint32_t(new_separator);
  };

  // Begins with a separator
  if (is_separator(*it)) {
    append_safe(static_cast<uint32_t>(new_separator), result);
    ++it;
    last = it;
  }

  for (;;) {
    it = std::find_if(it, path_end, is_separator);
    if (it == path_end) {
      break;
    }

    if (it != last) {
      auto offset = byte_length(path_begin, last);
      auto length = byte_length(path_begin, it) - offset;
      result += path.substr(offset, length);
      append_safe(static_cast<uint32_t>(new_separator), result);
    }

    ++it;
    last = it;
  }

  if (last == path_begin) {
    return std::string(path);
  }

  if (last != path_end) {
    auto offset = byte_length(path_begin, last);
    result += path.substr(offset);
  }

  return result;
}

std::string utf8_find_name_from_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = make_citer(path);

  auto it = end;
  --it;

  // skip trailing separators at the end of the path
  while (*it == uint32_t(separator)) {
    if (it == begin) {
      // path is all separators, name is empty
      return std::string();
    }
    --it;
  }

  // update end so it is before any trailing separators
  end = std::next(it);

  // skip non-separators
  while (*it != uint32_t(separator)) {
    if (it == begin) {
      break;
    }
    --it;
  }

  // if the iterator is on a separator, advance
  if (*it == uint32_t(separator)) {
    ++it;
  }

  auto offset = byte_length(begin, it);
  auto length = byte_length(it, end);
  return std::string(path.substr(offset, length));
}

std::string utf8_find_base_name_from_path(const std::string_view path, char32_t separator) {
  auto name = utf8_find_name_from_path(path, separator);
  if (!name.length()) {
    return std::string();
  }

  auto [begin, end] = make_citer(name);

  auto it = end;
  --it;

  while (it != begin) {
    if (*it == uint32_t('.')) {
      break;
    }
    --it;
  }

  if (it == begin) {
    return name;
  }

  auto length = byte_length(it, end);
  auto offset = name.length() - length;
  return std::string(name.substr(0, offset));
}

std::string utf8_find_base_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto [begin, end] = make_citer(path);

  auto it = end;
  --it;

  // skip trailing separators at the end of the path
  while (*it == uint32_t(separator)) {
    if (it == begin) {
      return std::string();
    }
    --it;
  }

  // skip non-separators
  while (*it != uint32_t(separator)) {
    if (it == begin) {
      // there are no separators, base path is empty
      return std::string();
    }
    --it;
  }

  // Save position of the separator we just found
  auto sep_pos = it;

  // skip trailing separators at the end of the base path
  while (*it == uint32_t(separator)) {
    if (it == begin) {
      // base path is all separators, base path is empty
      return std::string();
    }
    --it;
  }

  // If we stopped at a colon (drive letter like "D:"), include the root separator
  // This ensures "D:\file.txt" returns "D:\" not "D:"
  if (*it == uint32_t(':')) {
    auto length = byte_length(begin, std::next(sep_pos));
    return std::string(path.substr(0, length));
  }

  auto length = byte_length(begin, std::next(it));
  return std::string(path.substr(0, length));
}

std::string utf8_canonicalize_path(const std::string_view path, char32_t separator) {
  if (path.empty()) {
    return std::string();
  }

  auto is_rooted = utf8_starts_with(path, separator);

  auto parts = utf8_split_path(path);
  for (auto it = parts.begin(); it != parts.end();) {
    const auto& part = *it;
    if (part == ".") {
      // Potential marker for current directory.
      it = parts.erase(it);
    } else if (part == "..") {
      // Ensure we don't override the device name.
      if (it != parts.begin()) {
        auto prev = std::prev(it);
        if (!utf8_ends_with(*prev, ":")) {
          it = parts.erase(prev);
        }
      }
      it = parts.erase(it);
    } else {
      ++it;
    }
  }

  return !is_rooted ? utf8_join_paths(parts, separator)
                    : to_string(separator) + utf8_join_paths(parts, separator);
}

}  // namespace rex::string
