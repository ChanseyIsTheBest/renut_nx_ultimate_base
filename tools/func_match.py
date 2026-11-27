#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): finds this game's counterparts of another recompiled game's functions by the shape of their
code (for CRI's MPV decoder: sasrt-nx's Sofdec hook names its functions; this game's MPV is an older version of the same
library). Each function's PowerPC words are masked of what moves between builds (branch targets, the 16-bit immediates
and displacements of D-form instructions: addresses and struct offsets), keeping the opcodes, the extended opcodes and
the register fields; this game's functions of a similar length are ranked by difflib's ratio over those words.

  python tools/func_match.py --their-image <image.bin> --their-init <x_init.cpp> --our-image out/renut_image.bin
                             --our-init app/generated/renut_init.cpp 82EA4F08 82EA4B88 ...
Images are memory images based at 0x82000000 (offset = address - 0x82000000).
"""
import argparse
import difflib
import re
import struct

BASE = 0x82000000


def starts(init_path):
    out = []
    for m in re.finditer(r'\{ 0x([0-9A-F]{8}), sub_[0-9A-F]{8} \}', open(init_path, encoding='utf-8', errors='replace').read()):
        out.append(int(m.group(1), 16))
    return sorted(set(out))


def words(img, start, end):
    return list(struct.unpack('>%dI' % ((end - start) // 4), img[start - BASE:end - BASE]))


def mask(w):
    op = w >> 26
    if op in (18,):          # b / bl: the target
        return w & 0xFC000003
    if op == 16:             # bc: the target
        return w & 0xFFFF0003
    if op in (14, 15, 24, 25, 26, 27, 28, 29, 7, 8, 10, 11, 12, 13) or 32 <= op <= 55 or op in (58, 62):
        return w & 0xFFFF0000  # D-form immediates / displacements
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--their-image', required=True)
    ap.add_argument('--their-init', required=True)
    ap.add_argument('--our-image', required=True)
    ap.add_argument('--our-init', required=True)
    ap.add_argument('--top', type=int, default=3)
    ap.add_argument('--slack', type=float, default=0.35, help='length difference allowed (fraction)')
    ap.add_argument('functions', nargs='+')
    a = ap.parse_args()
    timg, oimg = open(a.their_image, 'rb').read(), open(a.our_image, 'rb').read()
    tst, ost = starts(a.their_init), starts(a.our_init)
    ours = []
    for i, s in enumerate(ost[:-1]):
        e = ost[i + 1]
        if e - s > 4 * 20000:
            continue
        ours.append((s, [mask(w) for w in words(oimg, s, e)]))
    for f in a.functions:
        s = int(f, 16)
        i = tst.index(s)
        e = tst[i + 1]
        want = [mask(w) for w in words(timg, s, e)]
        n = len(want)
        cands = []
        for os_, ow in ours:
            if abs(len(ow) - n) > max(4, a.slack * n):
                continue
            sm = difflib.SequenceMatcher(None, want, ow, autojunk=False)
            if sm.real_quick_ratio() < 0.5 or sm.quick_ratio() < 0.5:
                continue
            cands.append((sm.ratio(), os_, len(ow)))
        cands.sort(reverse=True)
        best = ', '.join(f'sub_{c[1]:08X} ({c[2]} words, {c[0]:.3f})' for c in cands[:a.top]) or 'none'
        print(f'sub_{s:08X} ({n} words): {best}', flush=True)


if __name__ == '__main__':
    main()
