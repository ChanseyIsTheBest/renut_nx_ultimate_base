/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/xmemory.h>

// renut-ultimate (2026-11-16, port 146; tw2de_nx's, from nx-mod/rexglue-sdk-nx; off by default at the user's word).
REXCVAR_DEFINE_BOOL(crt_xmemcpy_as_memmove, false, "Kernel",
                    "tw2de_nx (exact): the native XMemCpy (rexcrt_XMemCpy, which a game maps its own block copy to) "
                    "is a memmove: a game's forward block copy may be called with overlapping ranges, which memcpy "
                    "leaves undefined. false: memcpy, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// dmc_devil-nx (517b4928, DMC-11; its notes: the "mutating CRT hooks in sdk/src/kernel/crt/memory.cpp called
// MarkStoreCardsHost before writing payload memory without a release fence"). Its comment: "publish the copied/filled
// bytes before zeroing store cards so PagesClean cannot observe a zeroed card on the render thread before the payload
// write lands". Renut's PagesClean sets a written page's card back to 1 when it sees it: if it does so between a
// hook's card and its bytes, it converts the old bytes and the card no longer says the page changed, so the cached copy
// stays old until the page is written again. With the bytes first (and a store-store barrier, so the card cannot become
// visible before them), a check that falls inside the copy sees the card at the end, and the copy is converted again.
// OFF: the card first, as before. OFF by default: a barrier per hooked copy on the console, not measured; it only
// matters when a game writes pages the render thread is checking at that moment (DmC with frames ahead; the port
// verified it on the PC together with banjo_renut_kick_reuse = false and blur_renut_wipe_overwritten = true).
REXCVAR_DEFINE_BOOL(dmc_crt_cards_after_write, false, "Kernel",
                    "dmc_devil-nx: the CRT memory hooks (memcpy, memmove, memset, XMemCpy, XMemSet and kin) mark their "
                    "store cards after writing, behind a store barrier, so a render-thread check during the copy cannot "
                    "leave Renut's cached copy of the old bytes proven. false: the cards first, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::crt {

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

// banjo-nx (2026-10-19): every write of guest memory here marks its store cards (rex/system/xmemory.h), as the guest
// routines' stores would have.
using rex::memory::MarkStoreCardsHost;

// dmc_crt_cards_after_write, read once: after the first call a relaxed byte load (these hooks are hot, and a
// function-local static would add its guard's load-acquire to each call).
static std::atomic<int8_t> g_cards_after_write{-1};
static inline bool CardsAfterWrite() {
  int8_t v = g_cards_after_write.load(std::memory_order_relaxed);
  if (v < 0) {
    v = REXCVAR_GET(dmc_crt_cards_after_write) ? 1 : 0;
    g_cards_after_write.store(v, std::memory_order_relaxed);
  }
  return v != 0;
}

// The bytes, then the cards: a store-store barrier between them (AArch64 dmb ishst; a release fence elsewhere, which is
// a compiler barrier alone on x86).
static inline void MarkStoreCardsAfterWrite(const void* dst, size_t n) {
  if (!n) return;  // MarkStoreCardsHost marks nothing either
#if defined(__aarch64__)
  __asm__ volatile("dmb ishst" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_release);
#endif
  MarkStoreCardsHost(dst, n);
}

static void* native_memcpy(void* dst, const void* src, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memcpy(dst, src, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memcpy(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memmove(dst, src, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memset(dst, val, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    static const bool as_memmove_after = REXCVAR_GET(crt_xmemcpy_as_memmove);
    void* r = as_memmove_after ? std::memmove(dst, src, n) : std::memcpy(dst, src, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  // Port 146 (crt_xmemcpy_as_memmove): a game's own block copy hooked here may be called with overlapping ranges
  // that its forward copy handled.
  static const bool as_memmove = REXCVAR_GET(crt_xmemcpy_as_memmove);
  if (as_memmove)
    return std::memmove(dst, src, n);
  return std::memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memset(dst, val, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memset(dst, val, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    void* r = std::memset(dst, val, n);
    MarkStoreCardsAfterWrite(dst, n);
    return r;
  }
  MarkStoreCardsHost(dst, n);
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    std::memcpy(dst, src, count);
    MarkStoreCardsAfterWrite(dst, count);
    return 0;
  }
  MarkStoreCardsHost(dst, count);
  std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  if (CardsAfterWrite()) {  // dmc_crt_cards_after_write
    std::memmove(dst, src, count);
    MarkStoreCardsAfterWrite(dst, count);
    return 0;
  }
  MarkStoreCardsHost(dst, count);
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

REX_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
REX_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
REX_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
REX_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
REX_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
