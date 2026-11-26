/**
 * @file        platform/dynlib.h
 * @brief       Platform-agnostic dynamic library loading
 */
#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/platform.h>

namespace rex::platform {

enum class SymbolResolution {
  // Resolve symbols on first use. Maps to RTLD_LAZY on POSIX; the only mode on
  // Windows.
  kLazy,
  // Resolve all symbols at load time. Load fails if any unresolved symbol
  // exists. Maps to RTLD_NOW on POSIX; the only mode on Windows.
  kImmediate,
};

class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  bool Load(const std::filesystem::path& path, SymbolResolution mode = SymbolResolution::kLazy);
  void Close();
  explicit operator bool() const { return handle_ != nullptr; }

  void* GetRawSymbol(const char* name) const;

  template <typename T>
  T GetSymbol(const char* name) const {
    return reinterpret_cast<T>(GetRawSymbol(name));
  }

 private:
  void* handle_ = nullptr;
};

namespace lib_names {

#if REX_PLATFORM_WIN32

inline constexpr const char* kVulkanLoader = "vulkan-1.dll";
inline constexpr const char* kRenderDoc = "renderdoc.dll";
inline constexpr const char* kSpirvToolsSdkPath = "Bin/SPIRV-Tools-shared.dll";

#elif REX_PLATFORM_SWITCH

// There is no dynamic loading on Switch: everything is linked statically.
inline constexpr const char* kVulkanLoader = "";
inline constexpr const char* kRenderDoc = "";
inline constexpr const char* kSpirvToolsSdkPath = "";
// dmc_devil-nx: a game's second XEX linked into the NRO is found by its library name once the app has registered it
// (RexSwitchRegisterStaticModule, below; sdk/src/core/dynlib_switch.cpp).

#elif REX_PLATFORM_ANDROID

inline constexpr const char* kVulkanLoader = "libvulkan.so";
inline constexpr const char* kRenderDoc = "libVkLayer_GLES_RenderDoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_LINUX

inline constexpr const char* kVulkanLoader = "libvulkan.so.1";
inline constexpr const char* kRenderDoc = "librenderdoc.so";
inline constexpr const char* kSpirvToolsSdkPath = "bin/libSPIRV-Tools-shared.so";

#elif REX_PLATFORM_MAC

inline constexpr const char* kVulkanLoader = "libvulkan.1.dylib";
inline constexpr const char* kRenderDoc = "librenderdoc.dylib";
inline constexpr const char* kSpirvToolsSdkPath = "lib/libSPIRV-Tools-shared.dylib";

#else
#error No library names provided for the target platform.
#endif

}  // namespace lib_names

}  // namespace rex::platform

#if REX_PLATFORM_SWITCH
// dmc_devil-nx (4858b474): registers a game module linked into the NRO under its library name (KernelState's
// shared_lib_name, "<project>_<target>", e.g. "renut_NTJobCode") with its ReXModule_Register and ReXModule_GetImageInfo.
// Call it before the runtime loads modules (the app's OnPreSetup). A null or empty name, a name of 64 bytes or more, a
// null function or a full table (16) is refused with a warning; dmc_static_modules = false ignores the table.
extern "C" void RexSwitchRegisterStaticModule(const char* name, void* register_fn, void* image_info_fn);
#endif
