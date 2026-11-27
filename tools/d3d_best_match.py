#!/usr/bin/env python3
"""ssasr-nx (2026-11-25, from ilomilo-nx's): the functions of this game most like a reference function, by the token
alignment of tools/d3d_propagate.py (difflib ratio over opcode / register tokens), for the hooks the propagation left
unpaired. For each reference address it prints the best few over the whole code, and the best few in the window
between the ssasr pairs of its nearest paired neighbours in the reference (the XDK's link order is kept, so a D3D
function sits between its neighbours' counterparts).

  python -I tools/d3d_best_match.py --ref nb ADDR [ADDR ...] [--top 4]
"""
import argparse
import bisect
import difflib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_propagate import Img, starts_of, REFS, TGT_CODE, ROOT  # noqa: E402


def score(rt, T, cands):
    best = []
    for t in cands:
        tt = T.f[t][1]
        if not 0.5 * len(rt) <= len(tt) <= 2.0 * len(rt) + 4:
            continue
        sm = difflib.SequenceMatcher(None, rt, tt, autojunk=False)
        if sm.real_quick_ratio() < 0.45 or sm.quick_ratio() < 0.45:
            continue
        best.append((sm.ratio(), t, len(tt)))
    best.sort(reverse=True)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--ref', choices=sorted(REFS), default='nb')
    ap.add_argument('--top', type=int, default=4)
    ap.add_argument('--no-global', action='store_true')
    a = ap.parse_args()
    cfg = REFS[a.ref]
    R = Img(os.path.join(cfg['dir'], cfg.get('image', 'out/renut_image.bin')),
            starts_of(os.path.join(cfg['dir'], cfg.get('gen', 'app/generated'))), cfg['code'][0], cfg['code'][1])
    T = Img(os.path.join(ROOT, 'out', 'renut_image.bin'), starts_of(os.path.join(ROOT, 'app', 'generated')),
            TGT_CODE[0], TGT_CODE[1])
    pairs = {int(k, 16): int(v['ssasr'], 16) for k, v in
             json.load(open(os.path.join(ROOT, 'out', 'renut_map', f'{a.ref}_to_ssasr.json'))).items()}
    pk = sorted(pairs)
    tstarts = sorted(T.f)
    for s in a.addrs:
        r = int(s, 16)
        if r not in R.f:
            print(f'{r:08X}: not a {a.ref} function start')
            continue
        rt = R.f[r][1]
        line = f'{r:08X} ({len(rt)} insns)'
        # neighbours: the nearest paired functions below and above whose targets are ordered
        i = bisect.bisect_left(pk, r)
        lo = hi = None
        for j in range(i - 1, max(i - 40, -1), -1):
            if pk[j] != r:
                lo = (pk[j], pairs[pk[j]])
                break
        for j in range(i, min(i + 40, len(pk))):
            if pk[j] != r:
                hi = (pk[j], pairs[pk[j]])
                break
        if lo and hi and lo[1] < hi[1] and hi[1] - lo[1] < 0x8000:
            win = [t for t in tstarts if lo[1] < t < hi[1]]
            b = score(rt, T, win)
            line += (f' | window {lo[1]:08X}..{hi[1]:08X} ({len(win)} fns; ref {lo[0]:08X}..{hi[0]:08X}): ' +
                     ', '.join(f'{t:08X} {q:.3f} ({n})' for q, t, n in b[:a.top]))
        else:
            line += f' | no ordered window ({lo}, {hi})'
        if not a.no_global:
            b = score(rt, T, T.f)
            line += ' | global: ' + ', '.join(f'{t:08X} {q:.3f} ({n})' for q, t, n in b[:a.top])
        print(line)


if __name__ == '__main__':
    main()
