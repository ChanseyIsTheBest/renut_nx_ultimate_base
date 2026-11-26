// renut-ultimate (dmc_devil-nx's static modules, dmc_static_modules): the Switch's table of game modules linked into
// the NRO (sdk/src/core/static_module_registry.h), on a PC: registration, re-registration, lookup by library name with
// and without an ending, the two symbols, refusals, a full table, and that an entry never moves (a Load's handle stays
// good while later modules are registered).
//   clang++ -O2 -std=c++20 sdk/tests/static_module_registry_test.cpp -o out/tools/static_module_registry_test.exe
#include <cstdio>
#include <string>

#include "../src/core/static_module_registry.h"

namespace sm = rex::platform::static_modules;

static int g_failures = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
      ++g_failures;                                                 \
    }                                                               \
  } while (0)

static int f_reg, f_info, f_reg2, f_info2;

int main() {
  sm::Registry r;
  void* reg = &f_reg;
  void* info = &f_info;
  CHECK(sm::Find(r, "renut_NTJobCode") == nullptr);
  CHECK(sm::Add(r, "renut_NTJobCode", reg, info) == sm::AddResult::kAdded);
  const sm::Entry* e = sm::Find(r, "renut_NTJobCode");
  CHECK(e != nullptr);
  CHECK(sm::Find(r, "renut_NTJobCode.dll") == e);
  CHECK(sm::Find(r, "renut_NTJobCode.so") == e);
  CHECK(sm::Find(r, "renut_NTJobCod") == nullptr);
  CHECK(sm::Find(r, "renut_NTJobCode.exe") == nullptr);
  CHECK(sm::Find(r, "") == nullptr);
  CHECK(sm::Symbol(e, "ReXModule_Register") == reg);
  CHECK(sm::Symbol(e, "ReXModule_GetImageInfo") == info);
  CHECK(sm::Symbol(e, "ReXModule_Other") == nullptr);
  CHECK(sm::Symbol(e, nullptr) == nullptr);
  CHECK(sm::Symbol(nullptr, "ReXModule_Register") == nullptr);

  // Refusals change nothing.
  CHECK(sm::Add(r, nullptr, reg, info) == sm::AddResult::kBadName);
  CHECK(sm::Add(r, "", reg, info) == sm::AddResult::kBadName);
  CHECK(sm::Add(r, std::string(64, 'x').c_str(), reg, info) == sm::AddResult::kBadName);
  CHECK(sm::Add(r, std::string(63, 'y').c_str(), reg, info) == sm::AddResult::kAdded);
  CHECK(sm::Add(r, "renut_A", nullptr, info) == sm::AddResult::kNoFunction);
  CHECK(sm::Add(r, "renut_A", reg, nullptr) == sm::AddResult::kNoFunction);
  CHECK(sm::Find(r, "renut_A") == nullptr);

  // A re-registration replaces the functions in place.
  CHECK(sm::Add(r, "renut_NTJobCode", &f_reg2, &f_info2) == sm::AddResult::kUpdated);
  CHECK(sm::Find(r, "renut_NTJobCode") == e);
  CHECK(sm::Symbol(e, "ReXModule_Register") == &f_reg2);
  CHECK(sm::Symbol(e, "ReXModule_GetImageInfo") == &f_info2);

  // Filling the table: the first entry does not move, the 17th is refused.
  for (int i = 0; r.count.load() < sm::kMaxEntries; ++i) {
    CHECK(sm::Add(r, ("renut_M" + std::to_string(i)).c_str(), reg, info) == sm::AddResult::kAdded);
  }
  CHECK(sm::Add(r, "renut_Late", reg, info) == sm::AddResult::kFull);
  CHECK(sm::Find(r, "renut_Late") == nullptr);
  CHECK(sm::Find(r, "renut_NTJobCode") == e);
  CHECK(sm::Symbol(e, "ReXModule_Register") == &f_reg2);
  CHECK(sm::Find(r, "renut_M0") != nullptr);

  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
  return g_failures ? 1 : 0;
}
