#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the non-exact batch rows 4 and 5): tools/ssasr_native_gen.py's exact rewrites in every machine
copy, not only in the natives made from them.

  raw  a 16-byte vector loaded (lvx: byte-swapped into the register) and stored again unchanged (stvx: swapped back)
       is copied as its bytes; the register still gets the swapped value (dropped where nothing reads it).
  fwd  a stack word stored and loaded back with no label, call, jump or other store in between takes the stored value;
       the store stays. This is also PowerPC's integer <-> float conversion through the stack (std + lfd before fcfid,
       stfd + lwz after fctiwz): about 720 of each in the hot copies.

Both are exact by construction (ssasr_native_gen.py's docstring); the guard compares each copy's running form
(f_sub_X, the one these rewrite) with the original statements (o_sub_X, untouched). Run by tools/post_codegen.py after
the copies are made, before tools/ssasr_native_gen.py (which reads the rewritten copies: its rewrites are idempotent).
Environment SSASR_COPY_REWRITES=0: nothing (the copies as localize_native.py made them). Idempotent.

  python tools/ssasr_copy_rewrites.py [--hot app/generated/hot] [--check]
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ssasr_native_gen as gen  # noqa: E402

R1STORE = re.compile(r'^\s*BANJO_HOT_STORE_D_(?:U8|U16|U32|U64|F32|F64|F32ANY|F64ANY)\((l_r1|ll_r1)\.u32, (-?\d+), ')
WIDTH = {'U8': 1, 'U16': 2, 'U32': 4, 'U64': 8, 'F32': 4, 'F64': 8, 'F32ANY': 4, 'F64ANY': 8}
STACKY = re.compile(r'memset|memcpy|REX_DCBZ|STACK_FWD|REX_CHAIN_STORE|REX_STV|REX_DEAD_SAVE')


def loose_forward(out):
    """Row 3, the loose form (REX_SSASR_FWD_LOOSE, CMake SSASR_NON_EXACT): a stack word stored and loaded back takes the
    stored value across stores that do not go through r1 (the guest's own pointers: assumed not to point into the
    frame). Written as REX_SFWD_LOOSE(stored, load): the load itself without the switch (exact). The guard compares
    the copy with the original statements, so a store through a pointer into the frame shows as a difference."""
    n = len(out)
    count = 0
    for i in range(n):
        m = gen.SSTORE.match(out[i])
        if not m:
            continue
        width, reg_r1, off, src = m.group(2), m.group(3), int(m.group(4)), m.group(5)
        w = int(width) // 8
        for j in range(i + 1, min(n, i + 60)):
            l = out[j]
            s = l.strip()
            if s == 'if (s.Stopped()) return;':
                continue
            if gen.LABEL.match(s) or gen.CALLISH.search(l) or 'goto' in l or 'return' in l or s.startswith('REX_LOOP'):
                break
            ml = gen.SLOAD.match(l)
            if ml and ml.group(5) == reg_r1 and int(ml.group(6)) == off and ml.group(4) == width:
                dst = ml.group(2)
                val = f'{src}.u32' if width == '32' else f'{src}.u64'
                load = f'REX_LOAD_D_U{width}({reg_r1}.u32, {off})'
                out[j] = f'{ml.group(1)}{dst}.u64 = REX_SFWD_LOOSE({val}, {load});  /*lfwd*/'
                count += 1
                break
            if STACKY.search(l):
                break
            ms = R1STORE.match(l)
            if ms:  # a frame store: stop when it can overlap the word
                kind = re.match(r'^\s*BANJO_HOT_STORE_D_(\w+)\(', l).group(1)
                o2 = int(ms.group(2))
                if ms.group(1) != reg_r1 or not (o2 + WIDTH.get(kind, 16) <= off or off + w <= o2):
                    break
            elif 'REX_RAW_ADDR(ea)' in l and 'store' in l:
                # a vector store: through r1 when its ea was computed from it
                prev = next((out[k] for k in range(j - 1, i, -1) if gen.EA.match(out[k])), '')
                if 'r1' in prev:
                    break
            if re.match(r'^\s*(?:' + re.escape(src) + r'|' + reg_r1 + r')\.\w+ = ', l):
                break
    return out, count


HEAD = re.compile(r'^(\[\[[\w:, ]+\]\] )?static void f_sub_([0-9A-F]{8})\(PPCContext& __restrict ctx, uint8_t\* base, '
                  r'\[\[maybe_unused\]\] S& s\) \{$')
DECL = '\t[[maybe_unused]] simde__m128i {};  /*raw decl*/'


def patch(path, check):
    lines = open(path, encoding='utf-8').read().split('\n')
    totals = {'raw': 0, 'fwd': 0, 'lfwd': 0}
    changed = False
    k = 0
    while k < len(lines):
        m = HEAD.match(lines[k])
        if not m:
            k += 1
            continue
        end = next((j for j in range(k + 1, len(lines)) if lines[j] == '}'), None)
        if end is None:
            break
        body = lines[k + 1:end]
        declared = {re.search(r'simde__m128i (\w+);', l).group(1) for l in body if l.endswith('/*raw decl*/')}
        new, counts, raws = gen.rewrite(body)
        new, totals_l = loose_forward(new)
        totals['lfwd'] += totals_l
        add = [DECL.format(r) for r in raws if r not in declared]
        if new != body or add:
            new = add + new
            lines[k + 1:end] = new
            changed = True
            end = k + 1 + len(new)
        totals['raw'] += counts['raw']
        totals['fwd'] += counts['fwd']
        k = end + 1
    if changed and not check:
        open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    return changed, totals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hot', default=gen.HOT)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    if os.environ.get('SSASR_COPY_REWRITES', '1') == '0':
        print('ssasr_copy_rewrites: off (SSASR_COPY_REWRITES=0)')
        return
    files = changed = raw = fwd = lfwd = 0
    for path in sorted(glob.glob(os.path.join(a.hot, 'n_*.h'))):
        files += 1
        c, t = patch(path, a.check)
        changed += c
        raw += t['raw']
        fwd += t['fwd']
        lfwd += t['lfwd']
    print(f'ssasr_copy_rewrites: {files} copies, {changed} changed; {raw} vector copies as bytes, {fwd} stack loads '
          f'forwarded, {lfwd} more across stores not through r1 (REX_SFWD_LOOSE){" (check only)" if a.check else ""}')


if __name__ == '__main__':
    main()
