/**
 * @file        rex/core/dynlib_switch.cpp
 * @brief       DynamicLibrary backend for Horizon: there is no dynamic loading
 *
 * An NRO cannot load libraries at run time: dlopen does not exist and everything is
 * linked statically. This backend does not fail loudly; it returns "not loaded" for
 * any path, because the SDK's two callers already handle that case:
 *
 *   - gpu_plugin_loader.cpp, which on Switch must be resolved through static
 *     registration (see rex::system::LoadGpuPlugin)
 *   - the optional RenderDoc and SPIRV-Tools loaders, which make no sense on the
 *     console
 *
 * If a third caller that depends on dlopen is ever added, it will show up here as a
 * Load() that returns false, not as a link error.
 *
 * dmc_devil-nx (4858b474): the third caller is KernelState::LoadUserModule, for a game
 * with a second XEX (DmC's NTJobCode.xex). Such a module's recompiled code is linked into the NRO and the app registers
 * its two entry points by the module's library name (RexSwitchRegisterStaticModule, before the runtime loads modules:
 * OnPreSetup); Load then finds it (static_module_registry.h). Nothing registered, or dmc_static_modules = false: as
 * before, every Load returns false.
 */

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

#include "static_module_registry.h"

static_assert(REX_PLATFORM_SWITCH, "This file is Nintendo Switch-only");

REXCVAR_DEFINE_BOOL(dmc_static_modules, true, "Kernel",
                    "dmc_devil-nx: a game module the app linked into the NRO and registered "
                    "(RexSwitchRegisterStaticModule) is found by DynamicLibrary::Load under its library name. false: "
                    "every Load fails, as before (the module's functions do not run). Nothing changes for a game that "
                    "registers none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void RexSwitchRegisterStaticModule(const char* name, void* register_fn, void* image_info_fn) {
  namespace sm = rex::platform::static_modules;
  switch (sm::Add(sm::Get(), name, register_fn, image_info_fn)) {
    case sm::AddResult::kAdded:
      REXLOG_INFO("Static module '{}' registered", name);
      break;
    case sm::AddResult::kUpdated:
      REXLOG_INFO("Static module '{}' registered again (its functions replaced)", name);
      break;
    case sm::AddResult::kBadName:
      REXLOG_WARN("RexSwitchRegisterStaticModule: a missing, empty or too long name (63 bytes at most); not registered");
      break;
    case sm::AddResult::kNoFunction:
      REXLOG_WARN("RexSwitchRegisterStaticModule: '{}' without its two functions; not registered", name);
      break;
    case sm::AddResult::kFull:
      REXLOG_WARN("RexSwitchRegisterStaticModule: '{}' not registered: the table holds {} modules", name,
                  sm::kMaxEntries);
      break;
  }
}

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  (void)path;
  (void)mode;
  Close();
  // dmc_devil-nx: a module the app linked in and registered (static_module_registry.h).
  static const bool static_modules_on = REXCVAR_GET(dmc_static_modules);
  if (static_modules_on) {
    const std::string file_name = path.filename().string();
    if (const auto* entry = static_modules::Find(static_modules::Get(), file_name)) {
      handle_ = const_cast<static_modules::Entry*>(entry);
      return true;
    }
  }
  return false;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  (void)name;
  // dmc_devil-nx: handle_ is only ever set by Load above, to a registry entry (which never moves).
  if (handle_ && name) {
    return static_modules::Symbol(static_cast<const static_modules::Entry*>(handle_), name);
  }
  return nullptr;
}

}  // namespace rex::platform
