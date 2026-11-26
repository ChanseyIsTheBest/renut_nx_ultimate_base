#!/usr/bin/env python3
"""ilomilo-nx (2026-11-25): hooks the generated code bypasses.

tools/direct_calls.py (post_codegen) turns every call of a game function no app or SDK source hooks into a direct
`__imp__sub_X(ctx, base)` call, which the compiler may inline. A hook added or moved afterwards (a new address in a
REX_HOOK / BANJO_RENUT_HOOK line) is then never reached from those call sites until post_codegen runs again in full:
ilomilo's tiling hooks drew 256 lines of a tiled 720-line frame that way. `direct_calls.py --check` does not see it
(it counts calls still to convert). This lists, for each hooked address in the app's sources (registered forms only:
not BANJO_RENUT_UNHOOKED_*, not under `#if 0`), the direct `__imp__` calls of it in the generated code.

  python tools/hook_bypass_check.py [--gen app/generated] [--src app/src]
Exit 1 when any hook is bypassed.
"""
import argparse
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# renut-ultimate (port 291's hardening): REX_FUNC(sub_X) and BANJO_RENUT_YIELD_SHARED(sub_X) define sub_X too
# (app/src/me).
HOOK = re.compile(r'\b(?:REX_HOOK_RAW|REX_HOOK|REX_FUNC|BANJO_RENUT_HOOK(?:_RAW)?(?:_SHARED)?|BANJO_RENUT_YIELD_SHARED'
                  r'|BANJO_HOT_HOOK\w*)\s*\(\s*sub_([0-9A-Fa-f]{8})')


def hooked(src):
    out = {}
    for dirpath, _, files in os.walk(src):
        for fn in files:
            if not fn.endswith(('.cpp', '.h', '.inc')):
                continue
            path = os.path.join(dirpath, fn)
            # renut-ultimate (port 291's hardening): one entry per open #if, True while its branch is an `#if 0`'s; its
            # #else / #elif branch counts (before, a hook in the #else of an #if 0 was missed). Other conditions are not
            # evaluated: both branches count.
            skip = []
            for i, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
                s = line.strip()
                if s.startswith('#if'):
                    skip.append(bool(re.match(r'#if\s+0\b', s)))
                    continue
                if s.startswith(('#else', '#elif')):
                    if skip:
                        skip[-1] = False
                    continue
                if s.startswith('#endif'):
                    if skip:
                        skip.pop()
                    continue
                if any(skip) or s.startswith('//') or s.startswith('#define'):
                    continue
                for m in HOOK.finditer(line):
                    out.setdefault(m.group(1).upper(), f'{os.path.relpath(path, ROOT)}:{i}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--src', default=os.path.join(ROOT, 'app', 'src'))
    a = ap.parse_args()
    hooks = hooked(a.src)
    calls = collections.Counter()
    pat = re.compile(r'__imp__sub_([0-9A-Fa-f]{8})\(ctx')
    for path in glob.glob(os.path.join(a.gen, '*recomp*.cpp')) + glob.glob(os.path.join(a.gen, 'hot', '*.h')):
        for m in pat.finditer(open(path, encoding='utf-8', errors='replace').read()):
            calls[m.group(1).upper()] += 1
    bad = [(addr, calls[addr], where) for addr, where in sorted(hooks.items()) if calls.get(addr)]
    for addr, n, where in bad:
        print(f'sub_{addr}: {n} direct __imp__ calls in the generated code skip the hook at {where}')
    print(f'# {len(hooks)} hooked addresses, {len(bad)} bypassed by direct calls'
          + (': run tools/post_codegen.py in full' if bad else ''), file=sys.stderr)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
