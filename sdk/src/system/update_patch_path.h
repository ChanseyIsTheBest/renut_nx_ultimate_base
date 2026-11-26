/**
 * @file        system/update_patch_path.h
 * @brief       Where a title update keeps the patch of a module that lies in a subfolder of the game
 *
 * dmc_devil-nx (e3cbf45e; its notes, DMC-16: "fixed update:\<rel>p lookup in sdk/src/system/user_module.cpp", and
 * DMC-17: "fixed fs_entry->path() relative lookup"): a title update keeps each module's patch at the module's own path,
 * so DmC's TU1 has default.xexp at its root and NTJobCode.xexp under Binaries\NTJobCode\xenon\submission\, as the game
 * has NTJobCode.xex. UserModule::LoadFromFile looks next to the module, then at update:\<file>p (port 204, The Sims 3's
 * root-level patches); this is the third place: update:\<the module's path in its device>p.
 * Standard C++ only (sdk/tests/update_patch_path_test.cpp).
 */
#pragma once

#include <string>
#include <string_view>

namespace rex::system {

// update:\<entry_path>p for a module whose path in its device (Entry::path()) is entry_path and whose file name is
// file. Empty (nothing more to try) when that path is empty, is the file name alone (a module at the device's root:
// update:\<file>p was that place), or has a ".." part.
inline std::string UpdatePatchAtModulePath(std::string_view entry_path, std::string_view file) {
  size_t first = 0;
  while (first < entry_path.size() && (entry_path[first] == '\\' || entry_path[first] == '/')) ++first;
  std::string rel(entry_path.substr(first));
  for (char& c : rel) {
    if (c == '/') c = '\\';
  }
  if (rel.empty()) return {};
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
  if (rel.size() == file.size()) {
    bool same = true;
    for (size_t i = 0; i < rel.size() && same; ++i) same = lower(rel[i]) == lower(file[i]);
    if (same) return {};
  }
  size_t start = 0;
  while (start <= rel.size()) {
    const size_t end = rel.find('\\', start);
    const std::string_view part =
        std::string_view(rel).substr(start, (end == std::string::npos ? rel.size() : end) - start);
    if (part == "..") return {};
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return "update:\\" + rel + "p";
}

}  // namespace rex::system
