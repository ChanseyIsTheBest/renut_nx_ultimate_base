#!/usr/bin/env python3
"""pokersmash-nx (2026-11-24): lists a generated tree's functions in an address range with their instruction counts,
calls and the device displacements they use (r3-relative and others >= 10000), for pairing two games' Direct3D
functions by order by hand.

  python -I tools/func_range_list.py GEN LO HI [--names MAP.txt] [--min-disp 10000]

Poker Smash (2026-11-24): its Direct3D functions were paired with Katamari's by order where no signature placed them
(tools/renut_hook_map_*.txt's hand-placed lines). tools/d3d_align.py (port 328) aligns two XDKs' functions as a whole;
this is the listing to read beside it.

renut-ultimate (the hardening on the way in): --min-disp, 10000 by default as the port had it (Poker Smash's device
layout moved against Katamari's only from +10944 on, docs/pokersmash-notes.md's zones; a smaller value lists more).
"""
import argparse
import glob
import os
import re

DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$')
INSN = re.compile(r'^\t// ([a-z][a-z0-9.]*)\s*(.*)$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gen')
    ap.add_argument('lo')
    ap.add_argument('hi')
    ap.add_argument('--names')
    ap.add_argument('--min-disp', type=int, default=10000, help='list displacements at least this large')
    a = ap.parse_args()
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    names = {}
    if a.names:
        for line in open(a.names, encoding='utf-8-sig'):
            m = re.match(r'^([A-Za-z_]\w*)\s+([0-9A-F]{8})\b', line)
            if m:
                names[int(m.group(2), 16)] = m.group(1)
    funcs = {}
    for p in glob.glob(os.path.join(a.gen, '*.cpp')):
        cur = None
        for line in open(p, encoding='utf-8', errors='replace'):
            m = DEF.match(line)
            if m:
                ad = int(m.group(1), 16)
                cur = funcs.setdefault(ad, []) if lo <= ad < hi else None
                continue
            if cur is not None:
                m = INSN.match(line)
                if m:
                    cur.append((m.group(1), m.group(2)))
    for ad in sorted(funcs):
        ins = funcs[ad]
        calls = [o for mn, o in ins if mn == 'bl']
        disp = sorted({int(x) for mn, o in ins for x in re.findall(r',(-?\d+)\(r\d+\)', o) if abs(int(x)) >= a.min_disp})
        print(f'{ad:08X} n={len(ins):4} calls={len(calls):2} {names.get(ad, ""):34} disp={disp[:8]}')


if __name__ == '__main__':
    main()
