// banjo-nx (2026-10-22): what each shard of the bulk copies (banjo_hot_shard_<k>.cpp) includes before its copies,
// as banjo_hot_guest.cpp does for the hand-picked list: the generated code's macros (a copy is generated statements),
// the hook machinery (banjo_hot_guest.h) and the hand-written natives a copy asks first (hot/hand.txt).
// The method is bejeweled3-nx's (bulk localization, its docs/optimising-functions.md).
#pragma once

#include "renut_pch.h"

#include "cpu/banjo_hot_guest.h"
#include "cpu/hot/banjo_hand_natives.h"
// sasrt-nx (2026-11-11, port 74; its idea 39): store-card runs in the bulk copies (tools/localize_native.py,
// CARD_RUNS). A copy's function keeps the page of its run of stores (rex_card_run; 0xFFFFF: none pending) and marks a
// page's card when the run moves to another page; REX_CARD_FLUSH marks the pending one (before calls, fences and
// returns, and at the end) and clears it. A compare-and-swap marks the pending page and its own at once. With nothing
// pending a flush compiles to nothing (sasrt-nx's batch 2 audit, finding 1: a function that stores nothing marks
// nothing). Only for copies written with the runs (hot/banjo_hot_options.h: finding 2). The hand-written natives above
// keep marking at each store (their REX_CARD was expanded before this point).
#if __has_include("hot/banjo_hot_options.h")
#include "hot/banjo_hot_options.h"
#endif
#if defined(SASRT_HOT_CARD_RUNS) && defined(REX_CARD_RUNS_GENERATED) && !defined(REX_STORE_CARDS_OFF)
#undef REX_CARD
#undef REX_CARD_RUN_DECL
#undef REX_CARD_FLUSH
#undef REX_GUEST_CAS_C
#define REX_CARD_RUN_DECL [[maybe_unused]] u32 rex_card_run = 0xFFFFFu;
#define REX_CARD(ea)                                                                                      \
  ((void)((((u32)(ea)) >> 12) != rex_card_run                                                            \
              ? ((rex_card_run != 0xFFFFFu ? (void)(*(u8*)(base + rex_card_run) = 0) : (void)0),          \
                 rex_card_run = ((u32)(ea)) >> 12, 0)                                                     \
              : 0))
#define REX_CARD_FLUSH() \
  ((void)(rex_card_run != 0xFFFFFu ? (*(u8*)(base + rex_card_run) = 0, rex_card_run = 0xFFFFFu, 0) : 0))
#define REX_GUEST_CAS_C(ea, p, e, d) \
  (REX_CARD_FLUSH(), (void)(*(u8*)(base + (((u32)(ea)) >> 12)) = 0), REX_GUEST_CAS(p, e, d))
#endif
