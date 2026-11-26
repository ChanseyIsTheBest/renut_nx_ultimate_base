#!/usr/bin/env python3
"""pokersmash-nx (2026-11-24): the best matches in this game for given functions of another tree, by
tools/renut_sigmatch.py's field signature, within an address range (the Direct3D library's by default).

  python -I tools/renut_best_match.py --ref D:/renut-ultimate/base/app/generated --lo 82160000 --hi 82240000 sub_824EFBC8 [...]

Poker Smash (2026-11-24): its Renut hooks placed by signature against Katamari's functions took a score of 0.75 or
more, 0.05 ahead of the next (22 of them; its notes); this lists a function's best few by hand.

renut-ultimate (the hardening on the way in): --lo and --hi are required (the port's defaults were its own Direct3D
range), and a tree's functions that are not sub_X are left out of the pool.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from renut_sigmatch import load_tree, score  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--game', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--lo', required=True, help='the game\'s range to search, hex (its Direct3D library, say)')
    ap.add_argument('--hi', required=True)
    ap.add_argument('--top', type=int, default=4)
    ap.add_argument('funcs', nargs='+')
    a = ap.parse_args()
    ref, game = load_tree(a.ref), load_tree(a.game)
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    pool = {k: v for k, v in game.items()
            if re.fullmatch(r'sub_[0-9A-F]{8}', k) and lo <= int(k[4:], 16) < hi}
    for f in a.funcs:
        if f not in ref:
            print(f, 'not in the reference tree')
            continue
        best = sorted(((score(ref[f], v), k) for k, v in pool.items()), reverse=True)[:a.top]
        print(f, 'n=%d' % ref[f][3], '  '.join(f'{k}={s:.2f}(n={pool[k][3]})' for s, k in best))


if __name__ == '__main__':
    main()
