/**
 * @file        core/static_module_registry.h
 * @brief       The Switch's table of secondary game modules linked into the NRO (dynlib_switch.cpp)
 *
 * dmc_devil-nx (4858b474, "Phase 3: DmC Nintendo Switch deko3d NRO"; its notes: NTJobCode.xex "On Switch: statically
 * linked into renut.nro via dynlib_switch.cpp static module registration"): an NRO cannot load libraries, so a game's
 * second XEX (a codegen [[modules]] entry, KernelState::LoadUserModule's shared_lib_name, "<project>_<target>") is linked
 * in and its two entry points are registered here by name before the runtime loads modules. DynamicLibrary::Load then
 * finds the module by that name and GetRawSymbol hands out ReXModule_Register / ReXModule_GetImageInfo.
 *
 * renut-ultimate's hardening of the port's form: the entries live in a fixed array and never move (the port kept them in
 * a std::vector, whose growth would leave every handle_ a Load had handed out dangling), names are copied and bounded,
 * null entry points are refused, and the function pointers are atomics (a re-registration may overlap a lookup).
 * Header-only and standard C++ only, so sdk/tests/static_module_registry_test.cpp runs it on a PC.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string_view>

namespace rex::platform::static_modules {

struct Entry {
  char name[64];
  std::atomic<void*> register_fn;
  std::atomic<void*> image_info_fn;
};

inline constexpr size_t kMaxEntries = 16;

struct Registry {
  Entry entries[kMaxEntries]{};
  std::atomic<size_t> count{0};
  std::mutex mutex;
};

// The process's one table.
inline Registry& Get() {
  static Registry registry;
  return registry;
}

enum class AddResult { kAdded, kUpdated, kBadName, kNoFunction, kFull };

// Registers the module 'name' (a re-registration replaces its two functions).
inline AddResult Add(Registry& r, const char* name, void* register_fn, void* image_info_fn) {
  if (!name) return AddResult::kBadName;
  size_t len = 0;
  while (len < sizeof(Entry::name) && name[len]) ++len;  // bounded (strnlen is not in every C library's strict modes)
  if (len == 0 || len >= sizeof(Entry::name)) return AddResult::kBadName;
  if (!register_fn || !image_info_fn) return AddResult::kNoFunction;
  std::lock_guard<std::mutex> lock(r.mutex);
  const size_t n = r.count.load(std::memory_order_relaxed);
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(r.entries[i].name, name) == 0) {
      r.entries[i].register_fn.store(register_fn, std::memory_order_release);
      r.entries[i].image_info_fn.store(image_info_fn, std::memory_order_release);
      return AddResult::kUpdated;
    }
  }
  if (n >= kMaxEntries) return AddResult::kFull;
  Entry& e = r.entries[n];
  std::memcpy(e.name, name, len + 1);
  e.register_fn.store(register_fn, std::memory_order_relaxed);
  e.image_info_fn.store(image_info_fn, std::memory_order_relaxed);
  r.count.store(n + 1, std::memory_order_release);  // publishes the entry
  return AddResult::kAdded;
}

// The entry whose name is 'file_name', or 'file_name' without a ".dll", ".so" or ".nro" ending; nullptr otherwise
// (an empty name included: the Switch's lib_names are "").
inline const Entry* Find(const Registry& r, std::string_view file_name) {
  if (file_name.empty()) return nullptr;
  std::string_view stem = file_name;
  for (std::string_view ext : {std::string_view(".dll"), std::string_view(".so"), std::string_view(".nro")}) {
    if (stem.size() > ext.size() && stem.substr(stem.size() - ext.size()) == ext) {
      stem.remove_suffix(ext.size());
      break;
    }
  }
  const size_t n = r.count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n && i < kMaxEntries; ++i) {
    const std::string_view name(r.entries[i].name);
    if (name == file_name || name == stem) return &r.entries[i];
  }
  return nullptr;
}

// One of the two entry points the kernel asks a module library for (kernel_state.cpp), or nullptr.
inline void* Symbol(const Entry* e, const char* symbol) {
  if (!e || !symbol) return nullptr;
  if (std::strcmp(symbol, "ReXModule_Register") == 0) return e->register_fn.load(std::memory_order_acquire);
  if (std::strcmp(symbol, "ReXModule_GetImageInfo") == 0) return e->image_info_fn.load(std::memory_order_acquire);
  return nullptr;
}

}  // namespace rex::platform::static_modules
