#!/usr/bin/env python3
"""S4ep2-XBLA-nx (github.com/FurryLilManChild/S4ep2-XBLA-nx, GPL-3.0, 0.5.0; 2026-11-16): maps a reference game's Direct3D
functions onto ours by ORDER as well as shape.

d3d_match.py ranks every function of the image by shape alone, so the many tiny render-state accessors (identical
shapes, different immediates) land on whichever copy scores first. The library is linked in the same order in both
games, so this aligns the two function lists of a range (Needleman-Wunsch, score = shape ratio, gaps free) and reads
each reference function's partner off the alignment.

  python tools/d3d_align.py --ref-image R --ref-gen RG --ref-range 821B2000:821D6000 \
      --image out/default_image.bin --gen app/generated --range 823EC000:82413000 --names hooks.toml \
      [--pairs-out out/align_pairs.txt]

--names: a toml of the reference's hooks ('0xADDR = { name = "..." }' lines). S4ep2-XBLA-nx moved Sonic 4 Episode I's
298 Renut hooks (XDK 2.0.11626) to Episode II (XDK 2.0.20871) with it: 527 pairs over the Direct3D block, the tiny
render-state accessors placed that d3d_match.py could not; functions LTCG moved out of order (SetPredication, BeginTiling,
EndTiling, GetDeviceCaps) were placed by hand. The pairs feed tools/d3d_field_votes.py. Reads only the two images and
generated trees; writes only --pairs-out.
"""
import argparse, difflib, re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_match import function_starts, functions


def rng(s):
    a, b = s.split(':')
    return int(a, 16), int(b, 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref-image', required=True)
    ap.add_argument('--ref-gen', required=True)
    ap.add_argument('--ref-range', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--range', required=True)
    ap.add_argument('--names', required=True)
    ap.add_argument('--band', type=int, default=120)
    ap.add_argument('--pairs-out', default=None, help='write every aligned pair ("sub_REF sub_OURS ratio")')
    a = ap.parse_args()
    r0, r1 = rng(a.ref_range)
    o0, o1 = rng(a.range)
    rs = [s for s in function_starts(a.ref_gen) if r0 <= s < r1]
    os_ = [s for s in function_starts(a.gen) if o0 <= s < o1]
    R = functions(open(a.ref_image, 'rb').read(), rs)
    O = functions(open(a.image, 'rb').read(), os_)
    rs = [s for s in rs if s in R]
    os_ = [s for s in os_ if s in O]
    n, m = len(rs), len(os_)
    print(f'reference {n} functions, ours {m}', file=sys.stderr)
    NEG = -1e9
    score = [[NEG] * (m + 1) for _ in range(n + 1)]
    back = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        score[i][0] = 0
    for j in range(m + 1):
        score[0][j] = 0
    cache = {}
    def sim(i, j):
        a_, b_ = R[rs[i]], O[os_[j]]
        if not (0.5 <= len(b_) / len(a_) <= 2.0):
            return 0.0
        return difflib.SequenceMatcher(None, a_, b_, autojunk=False).ratio()
    for i in range(1, n + 1):
        c = int(i * m / n)
        for j in range(max(1, c - a.band), min(m, c + a.band) + 1):
            best, how = score[i - 1][j], 1
            if score[i][j - 1] > best:
                best, how = score[i][j - 1], 2
            s = sim(i - 1, j - 1)
            d = score[i - 1][j - 1] + (s if s >= 0.6 else -0.5)
            if d > best:
                best, how = d, 3
            score[i][j], back[i][j] = best, how
        # cells outside the band stay NEG except the borders
    i, j, pairs = n, m, {}
    # find best end cell in last row within band
    while i > 0 and j > 0:
        h = back[i][j]
        if h == 3:
            pairs[rs[i - 1]] = (os_[j - 1], sim(i - 1, j - 1))
            i, j = i - 1, j - 1
        elif h == 1:
            i -= 1
        elif h == 2:
            j -= 1
        else:
            break
    if a.pairs_out:
        with open(a.pairs_out, 'w') as f:
            for k in sorted(pairs):
                f.write(f'sub_{k:08X} sub_{pairs[k][0]:08X} {pairs[k][1]:.2f}\n')
    names = {int(x.group(1), 16): x.group(2) for x in
             re.finditer(r'(0x[0-9A-Fa-f]+)\s*=\s*\{\s*name\s*=\s*"([^"]+)"', open(a.names).read())}
    for addr, name in sorted(names.items()):
        if not (r0 <= addr < r1):
            continue
        p = pairs.get(addr)
        if p:
            print(f'{addr:08X} -> {p[0]:08X} {p[1]:.2f} {name}')
        else:
            print(f'{addr:08X} -> ?        ---- {name}')


if __name__ == '__main__':
    main()
