#!/usr/bin/env python3
"""ilomilo-nx (2026-11-24): the hooks in app/src that name a guest function by address, against this game's own
generated function table (app/generated/<project>_init.cpp).

The base's app is written for Beautiful Katamari's addresses (and some files for N&B's). A hook whose address is not
a function of this game fails at the link (its __imp__ has no definition) or is never reached; a hook whose address
happens to be a function START of this game silently replaces an unrelated function. This lists, per source file,
every hook definition (REX_HOOK, REX_HOOK_RAW, REX_FUNC, BANJO_RENUT_HOOK*, BANJO_RENUT_UNHOOKED*, BANJO_HOT_HOOK*,
BANJO_D3D_CENSUS*) and every REX_EXTERN(__imp__sub_X), with what X is here: START (a registered function: a
collision unless the hook was moved to this game), or absent.

  python tools/hook_collisions.py [--gen app/generated] [--src app/src] [--only-starts]
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOK = re.compile(r'\b(REX_HOOK_RAW|REX_HOOK|REX_FUNC|BANJO_RENUT_\w*HOOK\w*|BANJO_HOT_HOOK\w*|BANJO_D3D_CENSUS\w*'
                  r'|BANJO_RENUT_YIELD_SHARED)\s*\(\s*(?:__imp__)?sub_([0-9A-Fa-f]{8})')
EXTERN = re.compile(r'\bREX_EXTERN\s*\(\s*__imp__sub_([0-9A-Fa-f]{8})')


def registered(gen):
    starts = set()
    for name in os.listdir(gen):
        if name.endswith('_init.cpp'):
            with open(os.path.join(gen, name), encoding='utf-8', errors='replace') as f:
                for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8}),\s*sub_', f.read()):
                    starts.add(int(m.group(1), 16))
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--src', default=os.path.join(ROOT, 'app', 'src'))
    ap.add_argument('--only-starts', action='store_true')
    a = ap.parse_args()
    starts = registered(a.gen)
    if not starts:
        sys.exit('no function table found in ' + a.gen)
    total = {'START': 0, 'absent': 0}
    for dirpath, _, files in os.walk(a.src):
        for fn in sorted(files):
            if not fn.endswith(('.cpp', '.h', '.inc')):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, ROOT)
            with open(path, encoding='utf-8', errors='replace') as f:
                lines = f.readlines()
            for i, line in enumerate(lines, 1):
                s = line.lstrip()
                if s.startswith('//') or s.startswith('#define'):
                    continue
                for m in list(HOOK.finditer(line)) + list(EXTERN.finditer(line)):
                    addr = int(m.group(m.lastindex), 16)
                    kind = 'START' if addr in starts else 'absent'
                    total[kind] += 1
                    if a.only_starts and kind != 'START':
                        continue
                    what = m.group(1) if m.re is HOOK else 'REX_EXTERN'
                    print(f'{kind:6} {rel}:{i}: {what} sub_{addr:08X}')
    print(f'# {total["START"]} at a function start of this game, {total["absent"]} not a function here '
          f'({len(starts)} functions registered)', file=sys.stderr)


if __name__ == '__main__':
    main()
