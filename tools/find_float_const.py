#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): the generated functions that load a given float constant from the image (`lis rN,HI` then
`lfs fX,LO(rN)` / `addi rN,rN,LO` + `lfs fX,0(rN)` in the generated code's PowerPC comments), within an address range:
for finding library functions by the constants they compare with (XAudio2's SetFrequencyRatio: its 1/1024 minimum).

  python tools/find_float_const.py 0x3A800000 [--from 0x82354D50 --to 0x8238E278] [--gen app/generated] [--image ...]
"""
import argparse
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
LIS = re.compile(r'^\s*// lis r(\d+),(-?\d+)')
ADDI = re.compile(r'^\s*// addi r(\d+),r(\d+),(-?\d+)')
LFS = re.compile(r'^\s*// lf[sd]x? f\d+,(-?\d+)\(r(\d+)\)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('value', help='the float\'s bits, e.g. 0x3A800000')
    ap.add_argument('--from', dest='lo', default='0x82000000')
    ap.add_argument('--to', dest='hi', default='0x90000000')
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    a = ap.parse_args()
    want = int(a.value, 16)
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    img = open(a.image, 'rb').read()

    def word(addr):
        o = addr - BASE
        return struct.unpack('>I', img[o:o + 4])[0] if 0 <= o <= len(img) - 4 else None

    hits = {}
    for path in sorted(glob.glob(os.path.join(a.gen, 'renut_recomp.*.cpp'))):
        fn = None
        regs = {}
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                fn = int(m.group(1), 16)
                regs = {}
                continue
            if fn is None or not (lo <= fn < hi):
                continue
            m = LIS.match(line)
            if m:
                regs[int(m.group(1))] = (int(m.group(2)) << 16) & 0xFFFFFFFF
                continue
            m = ADDI.match(line)
            if m and int(m.group(2)) in regs:
                regs[int(m.group(1))] = (regs[int(m.group(2))] + int(m.group(3))) & 0xFFFFFFFF
                continue
            m = LFS.match(line)
            if m and int(m.group(2)) in regs:
                addr = (regs[int(m.group(2))] + int(m.group(1))) & 0xFFFFFFFF
                if word(addr) == want:
                    hits.setdefault(fn, set()).add(addr)
    for fn in sorted(hits):
        print(f'sub_{fn:08X}: loads {want:08X} from ' + ', '.join(f'{x:08X}' for x in sorted(hits[fn])))
    print(f'{len(hits)} functions')


if __name__ == '__main__':
    main()
