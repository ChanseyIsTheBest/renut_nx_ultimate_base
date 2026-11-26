#!/usr/bin/env python3
"""renut-ultimate (2026-11-27): does this game save and restore Direct3D state blocks? Scans a generated tree for
functions shaped like the XDK's state block capture and apply, so a port can tell in seconds whether pokersmash-nx's
native state block path (its hooks/state_block.cpp, left out of the base: bound to its addresses and XDK 2.0.6534's
layout; docs/off-by-default.md section 12) is worth porting to its game.

  python -I tools/state_block_scan.py [GEN] [--lo 82000000 --hi 83000000] [--min 5] [--max-n 800] [--top 10]

GEN defaults to app/generated. A function scores one point for each of the shape's six marks, read from the generated
code's instruction comments:
  li26     a loop of 26 (the textures, each AddRef'd in the capture)
  li4096   a 4,096-byte copy, its size in r5 (the shader constants)
  bctrl2   two or more indirect calls (the getters and setters through the device's method tables)
  cmp16    a compare with 16 (the stream sources)
  type13   compares with 1 and 3 (the block's type: all, pixel or vertex state)
  lhzx     a halfword table load (the state table's offsets)
A score of --min (5) or more in a function of at most --max-n (800) instructions is a candidate (Poker Smash's two are
175 and 172; a function of thousands collects marks by chance, and its listing says so). Poker Smash: its capture sub_82239650 scores 6 and its apply sub_82239A28
5, nothing else 5 or more; Katamari (this base's game): no candidate, its best in the Direct3D range 3 (frame timing,
not a state block). A candidate is a lead, not a proof: read its code, then measure its share of the draw thread before
writing a native form (Poker Smash's: 13.3 % of that thread in the copies, 37 % with their getters and setters).

Exit status: 0 with candidates, 1 with none, 2 on a bad argument or a tree with no generated function.
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\)')
MARKS = (
    ('li26', re.compile(r'// li r\d+,26\b')),
    ('li4096', re.compile(r'// li r5,4096\b')),  # the copy's size argument
    ('cmp16', re.compile(r'// cmplwi (?:cr\d,)?r\d+,16\b')),
    ('lhzx', re.compile(r'// lhzx ')),
)
CMP1 = re.compile(r'// cmplwi (?:cr\d,)?r\d+,1\b')
CMP3 = re.compile(r'// cmplwi (?:cr\d,)?r\d+,3\b')
BCTRL = re.compile(r'// bctrl\b')
INSN = re.compile(r'^\s*// ')


def marks_of(text):
    """The six marks of one function's generated text, and its instruction count."""
    m = {name: bool(rx.search(text)) for name, rx in MARKS}
    m['bctrl2'] = len(BCTRL.findall(text)) >= 2
    m['type13'] = bool(CMP1.search(text)) and bool(CMP3.search(text))
    n = sum(1 for line in text.splitlines() if INSN.match(line))
    return m, n


def scan(gen, lo, hi):
    """Every generated function in [lo, hi): (score, name, marks, instructions)."""
    out = []
    files = sorted(glob.glob(os.path.join(gen, '*.cpp')))
    for path in files:
        name, addr, lines = None, 0, []

        def done():
            if name is not None and lo <= addr < hi:
                m, n = marks_of(''.join(lines))
                out.append((sum(m.values()), name, m, n))

        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                hit = FUNC_RE.match(line)
                if hit:
                    done()
                    name, addr, lines = hit.group(1), int(hit.group(2), 16), []
                elif name is not None:
                    lines.append(line)
        done()
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('gen', nargs='?', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--lo', default='82000000', help='lowest address, hex')
    ap.add_argument('--hi', default='83000000', help='end of the range, hex (exclusive)')
    ap.add_argument('--min', type=int, default=5, help='the score of a candidate, 1-6')
    ap.add_argument('--max-n', type=int, default=800, help='the largest candidate, in instructions')
    ap.add_argument('--top', type=int, default=10, help='how many of the best to list')
    a = ap.parse_args(argv)
    try:
        lo, hi = int(a.lo, 16), int(a.hi, 16)
    except ValueError:
        print('--lo / --hi: hex addresses', file=sys.stderr)
        return 2
    if not (1 <= a.min <= 6) or lo >= hi or a.top < 1 or a.max_n < 1:
        print('--min is 1-6, --lo below --hi, --top and --max-n 1 or more', file=sys.stderr)
        return 2
    if not os.path.isdir(a.gen):
        print(f'{a.gen}: no such folder (the generated tree: run the code generator first)', file=sys.stderr)
        return 2
    found = scan(a.gen, lo, hi)
    if not found:
        print(f'{a.gen}: no generated function in {lo:08X}-{hi:08X}', file=sys.stderr)
        return 2
    found.sort(key=lambda r: (-r[0], r[1]))
    cands = [r for r in found if r[0] >= a.min and r[3] <= a.max_n]
    for score, name, m, n in found[:a.top]:
        flags = ' '.join(k for k, v in m.items() if v)
        note = ''
        if score >= a.min:
            note = '  <- candidate' if n <= a.max_n else f'  (over --max-n {a.max_n}: not a candidate)'
        print(f'{score} {name} n={n} {flags}{note}')
    print(f'{len(cands)} candidate(s) of {len(found)} functions scanned (score {a.min} or more, at most {a.max_n} '
          'instructions)')
    return 0 if cands else 1


if __name__ == '__main__':
    sys.exit(main())
