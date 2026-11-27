/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr,
                                                    uint32_t spin_count);
// ssasr-nx (2026-11-27): a guest critical section entered / left by host code on the calling guest thread (the title's
// mid-asm hooks). Enter returns false, taking nothing, for a section not initialised yet.
bool xeRtlEnterCriticalSectionAt(uint32_t cs_va);
void xeRtlLeaveCriticalSectionAt(uint32_t cs_va);

}  // namespace rex::kernel::xboxkrnl
