#!/usr/bin/env python3
"""ilomilo-nx (2026-11-24): the Direct3D device's field offsets, Beautiful Katamari (XDK 2.0.5632, the base's layout)
-> this game (ssasr-nx takes it from ilomilo-nx as is; its pairs from tools/renut_hook_remap_ssasr.py).

For each pair (out/renut_map/kat_ssasr_pairs.txt: the hooks placed by tools/renut_hook_remap_ilomilo.py), the two
instruction lists are aligned on their tokens (opcode and register fields, no immediates: difflib), and every aligned
pair of D-form loads / stores (base register not r1, displacement 2048 or more: the device's private part, or any
displacement with --all) is a vote "Katamari offset -> ilomilo offset". Prints each Katamari offset with its votes,
and the runs of offsets that move by the same amount (zones).

  python -I tools/device_offset_votes.py [--kat-image D:/renut-ultimate/base/out/renut_image.bin]
         [--kat-gen D:/renut-ultimate/base/app/generated] [--min 2048] [--out out/renut_map/device_votes.txt]
"""
import argparse
import collections
import difflib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_propagate import Img, starts_of, TGT_CODE, ROOT  # noqa: E402

DFORM = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu', 40: 'lhz',
         41: 'lhzu', 42: 'lha', 43: 'lhau', 44: 'sth', 45: 'sthu', 48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu',
         52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu', 14: 'addi'}


def dform(w):
    op = w >> 26
    if op in DFORM:
        ra = (w >> 16) & 31
        d = w & 0xFFFF
        if d & 0x8000:
            d -= 0x10000
        return DFORM[op], ra, d
    if op == 58 or op == 62:  # ld / std (DS form)
        ra = (w >> 16) & 31
        d = w & 0xFFFC
        if d & 0x8000:
            d -= 0x10000
        return ('ld' if op == 58 else 'std'), ra, d
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kat-image', default='D:/renut-ultimate/base/out/renut_image.bin')
    ap.add_argument('--kat-gen', default='D:/renut-ultimate/base/app/generated')
    ap.add_argument('--pairs', default=os.path.join(ROOT, 'out', 'renut_map', 'kat_ssasr_pairs.txt'))
    ap.add_argument('--min', type=int, default=2048)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut_map', 'device_votes.txt'))
    a = ap.parse_args()
    K = Img(a.kat_image, starts_of(a.kat_gen), 0x82000000, 0x83000000)
    T = Img(os.path.join(ROOT, 'out', 'renut_image.bin'), starts_of(os.path.join(ROOT, 'app', 'generated')),
            TGT_CODE[0], TGT_CODE[1])
    votes = collections.defaultdict(collections.Counter)
    where = collections.defaultdict(set)
    n = 0
    for line in open(a.pairs):
        p = line.split()
        if len(p) < 2:
            continue
        k, t = int(p[0][4:], 16), int(p[1][4:], 16)
        if k not in K.f or t not in T.f:
            continue
        n += 1
        kw, kt = K.f[k]
        tw, tt = T.f[t]
        sm = difflib.SequenceMatcher(None, kt, tt, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for o in range(i2 - i1):
                x, y = dform(kw[i1 + o]), dform(tw[j1 + o])
                if not x or not y or x[0] != y[0] or x[1] == 1:
                    continue
                if x[0] == 'addi' and x[1] == 0:
                    continue  # li
                if abs(x[2]) < a.min and abs(y[2]) < a.min:
                    continue
                votes[x[2]][y[2]] += 1
                where[(x[2], y[2])].add(p[2] if len(p) > 2 else p[0])
    out = []
    for kd in sorted(votes):
        c = votes[kd]
        best, bn = c.most_common(1)[0]
        tot = sum(c.values())
        alts = ', '.join(f'{v}x{m}' for v, m in c.most_common() if v != best)
        out.append(f'{kd:6d} -> {best:6d} ({best - kd:+5d})  {bn}/{tot}' + (f'  also {alts}' if alts else '') +
                   '  [' + ', '.join(sorted(where[(kd, best)])[:3]) + ']')
    open(a.out, 'w').write(f'# {n} pairs\n' + '\n'.join(out) + '\n')
    print(f'{n} pairs, {len(votes)} offsets voted; {a.out}')


if __name__ == '__main__':
    main()
