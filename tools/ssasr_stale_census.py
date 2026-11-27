#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, batch 4; the user: "implement all those censuses"): the stale-read census of the three
stack-frame elisions (CMake BANJO_GEN_NV_SAVES_ELIDE, BANJO_GEN_BACK_CHAIN_ELIDE, BANJO_GEN_LR_MOVES_ELIDE /
SASRT_HOT_LR_ELIDE). The elided stores leave the guest frame's old words in place; sa2-nx found a game reading such a
word (a frame word it never wrote: characters tinted). The guard cannot see it (the copies and the originals elide
alike).

With REX_SSASR_STALE_CENSUS (CMake SSASR_STALE_CENSUS, a PC census build) the block this step appends to the end of
the generated pch makes every elided store (REX_DEAD_SAVE, REX_CHAIN_STORE, REX_DEAD_LR_SAVE) write the poison word
0xDEADBEEF (0xDEADBEEFDEADBEEF for the 64-bit saves) instead of nothing, and every guest load of 4 or 8 bytes count
the reads that return it (REX_NX_CENSUS 50 / 51 / 52; RexStaleHit logs the first ones with the address). A read of
the poison is a read of a word the elision leaves stale. Without the definition the block is nothing.

Run by tools/post_codegen.py, last (after every step that writes the store and load macros). Idempotent.

  python tools/ssasr_stale_census.py --gen app/generated [--check]
"""
import argparse
import re
import glob
import os
import sys

MARKER = '// ssasr-nx: the stale-read census of the frame elisions (tools/ssasr_stale_census.py)'
END = '// (end of tools/ssasr_stale_census.py)\n'
BLOCK = MARKER + '''
#if defined(REX_SSASR_STALE_CENSUS) && defined(REX_NX_CENSUS_ON)
extern thread_local int g_rex_poison_store;
extern "C" void RexStaleHit(uint32_t ea, uint64_t value, int width);
static inline u32 rex_stale_load32_(u32 ea, u32 v) {
  if (v == 0xDEADBEEFu) [[unlikely]] { REX_NX_CENSUS(51); RexStaleHit(ea, v, 4); }
  return v;
}
static inline u64 rex_stale_load64_(u32 ea, u64 v) {
  if (v == 0xDEADBEEFDEADBEEFull) [[unlikely]] { REX_NX_CENSUS(52); RexStaleHit(ea, v, 8); }
  return v;
}
#undef REX_DEAD_SAVE
#define REX_DEAD_SAVE(statement) do { g_rex_poison_store = 1; statement; g_rex_poison_store = 0; } while (0)
#undef REX_CHAIN_STORE
#define REX_CHAIN_STORE(statement) do { g_rex_poison_store = 1; statement; g_rex_poison_store = 0; } while (0)
#undef REX_DEAD_LR_SAVE
#define REX_DEAD_LR_SAVE(statement) do { g_rex_poison_store = 1; statement; g_rex_poison_store = 0; } while (0)
#undef REX_STORE_U32
#define REX_STORE_U32(x, y) (*(REX_GUEST_VOLATILE u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = \\
    __builtin_bswap32(g_rex_poison_store ? (REX_NX_CENSUS(50), 0xDEADBEEFu) : (u32)(y)))
#undef REX_STORE_U64
#define REX_STORE_U64(x, y) (*(REX_GUEST_VOLATILE u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = \\
    __builtin_bswap64(g_rex_poison_store ? (REX_NX_CENSUS(50), 0xDEADBEEFDEADBEEFull) : (u64)(y)))
#undef REX_LOAD_U32
#define REX_LOAD_U32(x) rex_stale_load32_((u32)(x), \\
    __builtin_bswap32(*(REX_GUEST_VOLATILE u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))))
#undef REX_LOAD_U64
#define REX_LOAD_U64(x) rex_stale_load64_((u32)(x), \\
    __builtin_bswap64(*(REX_GUEST_VOLATILE u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))))
// The exit block's restores of the saved registers (marked _R by this step): their values are dead (the registers
// are C++ locals), so they read the slot without being counted.
#define REX_LOAD_D_U64_R(r, d) __builtin_bswap64(*(REX_GUEST_VOLATILE u64*)(base + (u32)(r) + (int32_t)(d)))
#define REX_LOAD_D_U32_R(r, d) __builtin_bswap32(*(REX_GUEST_VOLATILE u32*)(base + (u32)(r) + (int32_t)(d)))
#endif
#ifndef REX_LOAD_D_U64_R
#define REX_LOAD_D_U64_R(r, d) REX_LOAD_D_U64(r, d)
#define REX_LOAD_D_U32_R(r, d) REX_LOAD_D_U32(r, d)
#endif
''' + END
# A restore in a function's exit block: a load of a non-volatile register (r14-r31) or of r12 (the saved link
# register) from an r1-relative slot, among the lines right above a `return;` (comments, other restores, the stack
# pointer's own update, the link register's write).
RESTORE = re.compile(r'^(\t+(?:ll_|l_)?(?:r(?:1[2-9]|2\d|3[01]))\.u64 = )REX_LOAD_D_U(64|32)(\((?:ll_|l_)?r1l?\.u32, -?\d+\);.*)$')
EXIT_OK = re.compile(r'^\t+(//|(?:ll_|l_)?r1l?\.u64 = |REX_LR_WRITE|ctx\.lr = |if \(s\.Stopped\(\)\) return;|s\.Hit\(|REX_DEAD_LR)')


def mark_restores(lines):
    n = 0
    for i, l in enumerate(lines):
        if l.strip() != 'return;':
            continue
        for k in range(i - 1, max(-1, i - 40), -1):
            s = lines[k]
            m = RESTORE.match(s)
            if m:
                lines[k] = m.group(1) + 'REX_LOAD_D_U%s_R' % m.group(2) + m.group(3)
                n += 1
                continue
            if '_R(' in s and 'REX_LOAD_D_U' in s:
                continue
            if not EXIT_OK.match(s):
                break
    return n


def restore_text(text):
    return text.replace('REX_LOAD_D_U64_R(', 'REX_LOAD_D_U64(').replace('REX_LOAD_D_U32_R(', 'REX_LOAD_D_U32(')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated'))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restore', action='store_true',
                    help='the _R restores back to the plain loads (post_codegen.py runs this first: the other steps '
                         'know only the plain form)')
    a = ap.parse_args()
    sources = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))) + sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h')))
    if a.restore:
        back = 0
        for path in sources:
            text = open(path, encoding='utf-8').read()
            new = restore_text(text)
            if new != text:
                back += 1
                if not a.check:
                    open(path, 'w', encoding='utf-8', newline='\n').write(new)
        print(f'ssasr_stale_census --restore: {back} files back to the plain loads')
        return
    marked = 0
    for path in sources:
        lines = open(path, encoding='utf-8').read().split('\n')
        n = mark_restores(lines)
        if n:
            marked += n
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print(f'ssasr_stale_census: {marked} exit-block restores marked _R (not counted)')
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if not pchs:
        sys.exit('no *_pch.h in ' + a.gen)
    for p in pchs:
        text = open(p, encoding='utf-8').read()
        new = text
        if MARKER in new:
            start = new.index(MARKER)
            new = new[:start] + new[new.index(END, start) + len(END):]
        # last in the file, after every macro it replaces (the pch is #pragma once: no closing include guard)
        new = new.rstrip('\n') + '\n' + BLOCK
        if new != text and not a.check:
            open(p, 'w', encoding='utf-8', newline='\n').write(new)
        print(f'ssasr_stale_census: {p}: {"updated" if new != text else "unchanged"}{" (check only)" if a.check else ""}')


if __name__ == '__main__':
    main()
