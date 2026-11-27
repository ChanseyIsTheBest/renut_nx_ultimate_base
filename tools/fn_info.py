#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): what a guest address is, for placing hooks by hand. For each address: the registered function
that contains it (from app/generated/*_init.cpp), its size, the functions it calls (bl targets), its callers (bl into
its start, with each caller's own function), and the device offsets it touches (D-form loads / stores off r3 or a
register copied from r3 in its first instructions, a rough list). --dis N prints the first N instructions.

  python -I tools/fn_info.py ADDR [ADDR ...] [--dis 40] [--no-callers]
"""
import argparse
import bisect
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d_propagate import starts_of, ROOT, BASE, TGT_CODE  # noqa: E402

IMG = os.path.join(ROOT, 'out', 'renut_image.bin')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--dis', type=int, default=0)
    ap.add_argument('--no-callers', action='store_true')
    a = ap.parse_args()
    b = open(IMG, 'rb').read()
    starts = [s for s in starts_of(os.path.join(ROOT, 'app', 'generated')) if TGT_CODE[0] <= s < TGT_CODE[1]]

    def word(x):
        return struct.unpack_from('>I', b, x - BASE)[0]

    def fn_of(x):
        i = bisect.bisect_right(starts, x) - 1
        return starts[i] if i >= 0 else None

    def end_of(s):
        i = bisect.bisect_right(starts, s)
        return starts[i] if i < len(starts) else s + 0x100

    callers = {}
    if not a.no_callers:
        for x in range(TGT_CODE[0], TGT_CODE[1], 4):
            w = word(x)
            if w >> 26 == 18 and (w & 3) in (1, 0):
                d = w & 0x03FFFFFC
                if d & 0x02000000:
                    d -= 0x04000000
                t = (x + d) & 0xFFFFFFFF
                callers.setdefault(t, []).append((x, w & 1))
    for s in a.addrs:
        x = int(s, 16)
        f = fn_of(x)
        e = end_of(f)
        n = (e - f) // 4
        calls = []
        offs = set()
        for k in range(n):
            ad = f + 4 * k
            w = word(ad)
            if w >> 26 == 18 and w & 1:
                d = w & 0x03FFFFFC
                if d & 0x02000000:
                    d -= 0x04000000
                calls.append((ad + d) & 0xFFFFFFFF)
            op = w >> 26
            if op in (32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55):
                ra, d = (w >> 16) & 31, w & 0xFFFF
                if d >= 0x2000 and d < 0x8000 and ra != 1:
                    offs.add(d)
        print(f'{x:08X}: in sub_{f:08X} (+{x - f:#x}), {n} insns')
        print('  calls: ' + ' '.join(f'{c:08X}' for c in dict.fromkeys(calls)))
        if not a.no_callers:
            cl = callers.get(f, [])
            print(f'  callers ({len(cl)}): ' + ' '.join(f'{c:08X}{"" if l else "(b)"}<sub_{fn_of(c):08X}>' for c, l in cl[:30]))
        print('  big offsets: ' + ' '.join(str(o) for o in sorted(offs)))
        if a.dis:
            os.system(f'python -I "{os.path.join(ROOT, "tools", "ppc_dis.py")}" --image "{IMG}" {f:#x} {a.dis}')


if __name__ == '__main__':
    main()
