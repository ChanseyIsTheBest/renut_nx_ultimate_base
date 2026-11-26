// renut-ultimate (dmc_devil-nx's DMC-16, dmc_update_patch_module_path): sdk/src/system/update_patch_path.h, where a
// title update keeps the patch of a module in a subfolder of the game, on a PC.
//   clang++ -O2 -std=c++20 sdk/tests/update_patch_path_test.cpp -o out/tools/update_patch_path_test.exe
#include <cstdio>
#include <string>

#include "../src/system/update_patch_path.h"

static int g_failures = 0;
static void Check(const std::string& got, const std::string& want, int line) {
  if (got != want) {
    std::printf("FAIL line %d: '%s', want '%s'\n", line, got.c_str(), want.c_str());
    ++g_failures;
  }
}
#define CHECK_EQ(got, want) Check((got), (want), __LINE__)

int main() {
  using rex::system::UpdatePatchAtModulePath;
  // DmC's second module: its path in the game's device.
  CHECK_EQ(UpdatePatchAtModulePath("Binaries\\NTJobCode\\xenon\\submission\\NTJobCode.xex", "NTJobCode.xex"),
           "update:\\Binaries\\NTJobCode\\xenon\\submission\\NTJobCode.xexp");
  CHECK_EQ(UpdatePatchAtModulePath("\\Binaries\\NTJobCode.xex", "NTJobCode.xex"), "update:\\Binaries\\NTJobCode.xexp");
  CHECK_EQ(UpdatePatchAtModulePath("/Binaries/x/NTJobCode.xex", "NTJobCode.xex"), "update:\\Binaries\\x\\NTJobCode.xexp");
  // A module at the device's root: update:\<file>p was already that place.
  CHECK_EQ(UpdatePatchAtModulePath("default.xex", "default.xex"), "");
  CHECK_EQ(UpdatePatchAtModulePath("\\DEFAULT.XEX", "default.xex"), "");
  // Nothing to try.
  CHECK_EQ(UpdatePatchAtModulePath("", "default.xex"), "");
  CHECK_EQ(UpdatePatchAtModulePath("\\\\", "default.xex"), "");
  CHECK_EQ(UpdatePatchAtModulePath("..\\outside\\a.xex", "a.xex"), "");
  CHECK_EQ(UpdatePatchAtModulePath("Binaries\\..\\a.xex", "a.xex"), "");
  // ".." only as a whole part.
  CHECK_EQ(UpdatePatchAtModulePath("Bin..aries\\a.xex", "a.xex"), "update:\\Bin..aries\\a.xexp");
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
  return g_failures ? 1 : 0;
}
