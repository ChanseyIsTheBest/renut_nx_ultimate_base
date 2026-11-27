#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, batch 4 #28; the user: "Do all 50 in current batch please"): a hand native's call inside a
copy, without the reloads after it when the native ran.

The copier writes a call of a callee that has a hand native (banjo_hand_natives.h, inline) as one block:

  { ctx.r3 = l_r3; ...; REX_CARD_FLUSH(); if (!::banjo::hand::n_X::Native(ctx, base, s)) { C::sub_X(ctx, base, s);
    if (s.Stopped()) return true; } else if (s.Stopped()) return true; l_r1 = ctx.r1; ...; }

The stores make the context current for the callee (and stay: later code relies on it being current). The loads after
it take back what the callee may have changed. A hand native changes no register in the context (it reads its
arguments and writes guest memory alone: each Native's code), so after it those loads give each local the value it
already has: they are moved into the fallback branch, the only one that can change them:

  { ctx.r3 = l_r3; ...; REX_CARD_FLUSH(); if (!::banjo::hand::n_X::Native(ctx, base, s)) { C::sub_X(ctx, base, s);
    if (s.Stopped()) return true; l_r1 = ctx.r1; ...; } else { if (s.Stopped()) return true; <the reloads of the
    registers this block did not store>; } }

(a reload of a register the block did not store stays on both paths: the value the code had before, exactly).

Only for the natives listed in NATIVES (checked by hand: none writes the context). The engine-constants native
(sub_82468C28) makes 16 memcpy calls a draw this way. Idempotent; run by tools/post_codegen.py after
ssasr_native_gen.py.

  python tools/ssasr_native_reloads.py [--gen app/generated] [--check]
"""
import argparse
import glob
import os
import re

# Natives whose Native() writes no PPCContext field (read from banjo_hand_natives.h): the game's memcpy.
NATIVES = ['8216F670']

BLOCK = re.compile(
    r'\{ (?P<stores>(?:ctx\.\w+ = l_\w+; )*)REX_CARD_FLUSH\(\); if \(!::banjo::hand::n_(?P<n>[0-9A-F]{8})::Native\(ctx, base, s\)\) '
    r'\{ (?P<call>(?:C::sub_|f_sub_)(?P=n)\(ctx, base, s\); if \(s\.Stopped\(\)\) (?P<ret>return(?: true)?); )'
    r'\} else if \(s\.Stopped\(\)\) (?P=ret); (?P<loads>(?:l_\w+ = ctx\.\w+; )+)\}')
LOAD = re.compile(r'l_(\w+) = ctx\.(\w+); ')
STORE = re.compile(r'ctx\.(\w+) = l_(\w+); ')


def rewrite(text):
    n = 0

    def sub(m):
        nonlocal n
        if m.group('n') not in NATIVES:
            return m.group(0)
        stored = {(a, b) for a, b in STORE.findall(m.group('stores'))}
        # After the native, a local stored in this block holds what its reload would give it: only the others' reloads
        # stay on that path (what the code did before, exactly).
        kept = ''.join(f'l_{a} = ctx.{b}; ' for a, b in LOAD.findall(m.group('loads')) if (b, a) not in stored)
        n += 1
        return ('{ ' + m.group('stores') + 'REX_CARD_FLUSH(); if (!::banjo::hand::n_' + m.group('n') +
                '::Native(ctx, base, s)) { ' + m.group('call') + m.group('loads') + '} else { if (s.Stopped()) ' +
                m.group('ret') + '; ' + kept + '} }')

    return BLOCK.sub(sub, text), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    total = files = 0
    for path in sorted(glob.glob(os.path.join(a.gen, 'hot', '*.h'))):
        text = open(path, encoding='utf-8').read()
        new, n = rewrite(text)
        if n:
            total += n
            files += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    print(f'ssasr_native_reloads: {total} native calls in {files} files without their reloads on the native path'
          f'{" (check only)" if a.check else ""}')


if __name__ == '__main__':
    main()
