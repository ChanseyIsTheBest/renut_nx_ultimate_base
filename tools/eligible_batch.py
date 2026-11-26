#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25): the next batch of machine copies without samples (optimising-functions.md, "What to do
next" 1 and 2: every other eligible function, in batches, watching NRO size, free heap and build time).

Every generated function that tools/switch_hot_bulk.py would take if a profile had sampled it (the same rules: not a
tiny leaf, at least --min-instructions guest instructions, no app or SDK source naming it, not threaded dispatch,
localize_native.fits() content), ordered by its distance in the direct call graph from the functions already on the
bulk list (their callees first: the code that runs when the hot code runs), then by size (larger first). A function no
run reaches cannot be verified, so a batch goes through a PC verify run (tools/run_pc_verify.ps1) and the lines whose
copy was not reached, or differed, are marked `off` (tools/eligible_batch.py --keep-reached LOG).

  python tools/eligible_batch.py --count 400 [--depth 2] [--write]      # appends to app/src/cpu/hot/localized_bulk.txt
  python tools/eligible_batch.py --keep-reached out/pc_runs/<run>/<run>.log --batch b1 [--write]

The verify run needs banjo_hot_bulk_lines (tools/run_pc_verify.ps1 -Extra "--banjo_hot_bulk_lines=true"). Poker
Smash (2026-11-25): batch b1, 1,635 functions at distance 1: 766 copies reached in a 360 s PC verify, 1 difference
(an earlier copy, marked off), 501 of b1 kept; b2, 705 more: 152 kept, 0 differences; then every eligible function
("all": 7,125 on) with one Ryujinx guard run (0 differences in 39.8 M calls compared; 1,402 copies reached).

renut-ultimate (the hardening on the way in): a tools/post_codegen.py whose switch_goto line has moved says so (the
threaded-dispatch functions would no longer be left out), and --keep-reached --write keeps the list's last newline.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import localize_native as ln  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BULK = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'localized_bulk.txt')


def bulk_lines():
    return open(BULK, encoding='utf-8').read().split('\n')


def listed_in_bulk(lines):
    # the list's own lines, '# out:' lines, and a batch's copies that a verify run did not reach ('# b1, not reached
    # ...: XXXXXXXX ...'): tried already with the same play, so a later batch does not take them again
    return {('sub_' + m.group(1).upper()) for l in lines for m in [re.match(r'\s*([0-9A-Fa-f]{8})\b', l)] if m} | \
        {('sub_' + m.group(1).upper()) for l in lines for m in [re.match(r'#\s*out:\s*([0-9A-Fa-f]{8})\b', l)] if m} | \
        {('sub_' + m.group(1).upper()) for l in lines
         for m in [re.match(r'#[^:]*not reached[^:]*:\s*([0-9A-Fa-f]{8})\b', l)] if m}


def candidates(a):
    index = ln.Index(ln.GEN)
    listed = [e[0] for e in ln.list_entries(ln.LIST)]
    hand = ln.hand_entries()
    hooks = ln.Hooks(listed)
    sizes = ln.addresses(ln.GEN)
    lines = bulk_lines()
    have = listed_in_bulk(lines) | set(listed) | set(hand)
    import store_cards

    def raw_ok(sub):
        for _, l in index.body(sub):
            l = store_cards.restore_text(l)
            if 'REX_RAW_ADDR(' in l and not (ln.RAW_VECTOR_STORE.match(l) or ln.RAW_MEMSET.match(l) or
                                             ln.RAW_LOAD.match(l) or
                                             (ln.ATOMICS and (ln.ATOMIC_LOAD.match(l) or ln.ATOMIC_CAS.match(l)))):
                return False
        return True

    post = open(os.path.join(ROOT, 'tools', 'post_codegen.py'), encoding='utf-8').read()
    at = post.find("tool('switch_goto.py'), '--gen', gen, '--thread'")
    if at < 0:
        print('eligible_batch: WARNING: no switch_goto.py --thread line in tools/post_codegen.py: threaded-dispatch '
              'functions are not left out')
    threaded = set(re.findall(r'sub_[0-9A-F]{8}', post[at:][:300])) if at >= 0 else set()
    # distance from the copies in the direct call graph, both ways (callees and callers: a caller of hot code runs
    # whenever it calls it; the copies' own callees are nearly all copied or refused already)
    callers = collections.defaultdict(set)
    for s, f in index.funcs.items():
        for c in f['callees']:
            callers[c].add(s)
    # from the copies that are on (reached code), not from the tried-and-unreached ones
    roots = {('sub_' + m.group(1).upper()) for l in lines
             for m in [re.match(r'\s*([0-9A-Fa-f]{8})\s+(?!off\b)', l)] if m}
    depth = {s: 0 for s in roots if s in index.funcs}
    for s in have:
        depth.setdefault(s, -1)  # tried: never a candidate, never a root
    frontier = list(depth)
    for d in range(1, a.depth + 1):
        nxt = []
        for s in frontier:
            for c in list(index.funcs[s]['callees']) + sorted(callers.get(s, ())):
                if c in index.funcs and c not in depth:
                    depth[c] = d
                    nxt.append(c)
        frontier = nxt
    rows, left = [], collections.Counter()
    for sub, f in index.funcs.items():
        if sub in have:
            continue
        d = depth.get(sub, a.depth + 1)
        if a.depth_only and d > a.depth:
            continue
        ins = sizes.get(sub, (0, 0))[1] // 4
        if not f['callees'] and not f['unknown'] and ins <= a.tiny:
            left['tiny leaf'] += 1
            continue
        if ins < a.min_instructions:
            left['small'] += 1
            continue
        if hooks.replaced(sub):
            left['an app or SDK source mentions it'] += 1
            continue
        if sub in threaded:
            left['threaded dispatch'] += 1
            continue
        why = ln.fits(index, hooks, sub)
        if why == 'raw guest access' and raw_ok(sub):
            why = None
        if why:
            left[why.split(':')[0][:40]] += 1
            continue
        rows.append((d, ins, sub, ins, len(f['callees'])))  # nearer first, then smaller (more copies per MB)
    rows.sort()
    return rows, left, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--count', type=int, default=400)
    ap.add_argument('--depth', type=int, default=2, help='call-graph distance from the copies that counts as near')
    ap.add_argument('--depth-only', action='store_true', help='only functions within --depth')
    ap.add_argument('--tiny', type=int, default=12)
    ap.add_argument('--min-instructions', type=int, default=40)
    ap.add_argument('--batch', default='b1', help='the batch name written on each line')
    ap.add_argument('--keep-reached', help='a PC verify log: mark this batch\'s copies off unless reached with no '
                                           'difference')
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    if a.keep_reached:
        return keep_reached(a)
    rows, left, lines = candidates(a)
    take = rows[:a.count]
    by_depth = collections.Counter(d for d, *_ in rows)
    print(f'{len(rows)} eligible functions not yet copied (by distance from the copies: ' +
          ', '.join(f'{d}: {n}' for d, n in sorted(by_depth.items())) + '); left out: ' +
          ', '.join(f'{k} {v}' for k, v in left.most_common()))
    print(f'this batch ({a.batch}): {len(take)}, {sum(r[3] for r in take)} guest instructions')
    if not a.write:
        return
    out = [l for l in lines]
    while out and not out[-1].strip():
        out.pop()
    out.append(f'# {a.batch} (tools/eligible_batch.py, no samples: distance from the copies, then size)')
    for d, _, sub, ins, calls in take:
        out.append(f'{sub[4:]} {a.batch}: 0 samples, {ins} instructions, {calls} direct callees, distance {d}')
    open(BULK, 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
    print('appended to ' + os.path.relpath(BULK, ROOT))


def keep_reached(a):
    """The guard's report lines of a verify run (banjo_hot_bulk_lines): which bulk copies were reached, which differed."""
    text = open(a.keep_reached, encoding='utf-8', errors='replace').read()
    # "[hot] bulk reached sub_X on: N compared, M not comparable" (banjo_hot_bulk_lines = true in the run): reached
    # when either count is above 0 (a call that stops at a call outside the copy is compared up to the stop: its
    # stores before it).
    reached = {m.group(1).upper() for m in re.finditer(
        r'\[hot\] bulk reached sub_([0-9A-Fa-f]{8}) (?:on|off): (\d+) compared, (\d+) not comparable', text)
        if int(m.group(2)) + int(m.group(3)) > 0}
    if not reached:
        sys.exit('no "[hot] bulk reached" line in the log: run it with banjo_hot_bulk_lines = true')
    differed = {m.group(1).upper() for m in re.finditer(r'DIFFERENCE[^\n]*?sub_([0-9A-Fa-f]{8})', text)}
    lines = bulk_lines()
    out, kept, off = [], 0, 0
    for l in lines:
        m = re.match(r'([0-9A-Fa-f]{8}) ' + re.escape(a.batch) + r':', l)
        if m:
            addr = m.group(1).upper()
            if addr in reached and addr not in differed:
                kept += 1
            else:
                l = f'{addr} off {l[9:]}  # not reached in the verify run' if addr not in differed else \
                    f'{addr} off {l[9:]}  # the PC verify found a difference'
                off += 1
        out.append(l)
    print(f'{a.batch}: {kept} reached with no difference, {off} marked off')
    if a.write:
        text = '\n'.join(out)
        open(BULK, 'w', encoding='utf-8', newline='\n').write(text if text.endswith('\n') else text + '\n')


if __name__ == '__main__':
    main()
