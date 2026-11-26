#!/usr/bin/env python3
"""ilomilo-nx (2026-11-24): the argument registers each hooked Direct3D function reads, in the base's Katamari tree
(where each hook's signature was checked) and in this game's tree, side by side. An LTCG-linked Direct3D drops
arguments that are always the same (Sonic 4's CreateDevice and Present take none: the base's docs/xdk-notes.md), and
a later XDK adds some (SetGammaRamp's Flags at 11164 and later): a hook whose function reads other registers here
than in Katamari needs its signature checked before it runs.

Reads are counted from the guest instructions (the generator's `// mnemonic operands` comments, tools/reg_liveness.py's
defuse): a register r3-r10 / f1-f13 read before it is written, scanning in order up to the first call, return or
unconditional branch (a first approximation: a read behind a skipped branch is counted too).

  python -I tools/arg_regs_compare.py [--pairs out/renut_map/kat_game_pairs.txt] [--all]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from renut_sigmatch import load_tree  # noqa: E402
import reg_liveness as rl  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARGS = [rl.R(n) for n in range(3, 11)] + [rl.F(n) for n in range(1, 14)]
ARGMASK = sum(ARGS)


def entry_reads(ins):
    written = 0
    read = 0
    prev = ''
    for k, t in enumerate(ins):
        parts = t.split(None, 1)
        mn = parts[0]
        ops = parts[1] if len(parts) > 1 else ''
        base = mn.rstrip('.').rstrip('+-')
        # The prologue's register-save helper (`mflr r12; bl __savegprlr_N`) saves r14-r31: not a call that reads
        # arguments. Step over it.
        if base == 'bl' and k <= 2 and prev.startswith('mflr r12'):
            prev = t
            written |= rl.R(12)
            continue
        prev = t
        if base in ('bl', 'bctrl', 'blr', 'bctr', 'b') or base.startswith('bl') and base not in ('blt', 'ble'):
            if base in ('bl', 'bctrl', 'blrl'):
                break
            if base in ('blr', 'bctr', 'b'):
                break
        try:
            w, u = rl.defuse(mn, ops)
        except Exception:
            continue
        if u == rl.ALL:
            break
        read |= u & ~written & ARGMASK
        written |= w
    return read


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kat-gen', default='D:/renut-ultimate/base/app/generated')
    ap.add_argument('--pairs', default=os.path.join(ROOT, 'out', 'renut_map', 'kat_game_pairs.txt'))
    ap.add_argument('--all', action='store_true')
    a = ap.parse_args()
    K = load_tree(a.kat_gen)
    T = load_tree(os.path.join(ROOT, 'app', 'generated'))
    same = diff = 0
    for line in open(a.pairs):
        p = line.split()
        if len(p) < 2 or p[0] not in K or p[1] not in T:
            continue
        rk, rt = entry_reads(K[p[0]][4]), entry_reads(T[p[1]][4])
        tag = p[2] if len(p) > 2 else ''
        if rk == rt:
            same += 1
            if a.all:
                print(f'same  {tag:45} {p[0]} {p[1]}: {" ".join(rl.reg_names(rk))}')
            continue
        diff += 1
        print(f'DIFF  {tag:45} {p[0]} {p[1]}: Katamari {" ".join(rl.reg_names(rk)) or "-"} | this game '
              f'{" ".join(rl.reg_names(rt)) or "-"}')
    print(f'# {same} the same, {diff} different', file=sys.stderr)


if __name__ == '__main__':
    main()
