#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25): machine copies from a PC profile (docs/optimising-functions.md: "Any PC profile ... add
what it names to localized_bulk.txt. switch_hot_bulk.py reads ARM profiles only, so this is by hand"), with
tools/switch_hot_bulk.py's rules: no tiny leaves (at most --tiny guest instructions and no call: the generator inlines
them), nothing under --min-instructions (40) unless it has --small-keep-samples (50) samples (behind a hook a small
copy's callers no longer inline it), nothing an app or SDK source hooks, nothing tools/localize_native.py refuses,
nothing already on a list. Appends "<address> <what>" lines to the bulk list (--write) or prints them.

  python tools/pc_hot_bulk.py PC_PROFILE [--from-report 2] [--min-samples 1] [--write app/src/cpu/hot/localized_bulk.txt]

Poker Smash (2026-11-25): its PC profile's 110 sampled guest functions gave 96 copies, its wider one (--top 400) 133
more; each batch then went through a PC verify (tools/run_pc_verify.ps1) before it was kept.

renut-ultimate (the hardening on the way in): localize_native.fits() asked once per function, a list that does not
end in a newline gets one before the appended lines, and a profile with no guest function says so.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import localize_native as ln  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


_FILES = {}


def gen_lines(name):
    if name not in _FILES:
        _FILES[name] = open(os.path.join(ROOT, 'app', 'generated', name), encoding='utf-8',
                            errors='replace').read().split('\n')
    return _FILES[name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('--from-report', type=int, default=2)
    ap.add_argument('--min-samples', type=int, default=1)
    ap.add_argument('--tiny', type=int, default=12)
    ap.add_argument('--min-instructions', type=int, default=40)
    ap.add_argument('--small-keep-samples', type=int, default=50)
    ap.add_argument('--write')
    a = ap.parse_args()
    os.chdir(ROOT)
    out = subprocess.run([sys.executable, 'tools/pc_hot_functions.py', a.profile, '--from-report', str(a.from_report),
                          '--top', '5000', '--guest'], capture_output=True, text=True).stdout
    samples = {}
    for m in re.finditer(r'(sub_[0-9A-F]{8}) x(\d+)', out):
        samples[m.group(1)] = max(samples.get(m.group(1), 0), int(m.group(2)))
    if not samples:
        sys.exit(f'no guest function in tools/pc_hot_functions.py\'s answer for {a.profile} (--from-report?)')
    index = ln.Index(os.path.join('app', 'generated'))
    hooks = ln.Hooks([])
    listed = set()
    for p in ('app/src/cpu/hot/localized.txt', 'app/src/cpu/hot/localized_bulk.txt', 'app/src/cpu/hot/hand.txt'):
        for line in open(p, encoding='utf-8'):
            m = re.match(r'\s*(?:0x)?(?:sub_)?([0-9A-Fa-f]{8})\b', line)
            if m:
                listed.add('sub_' + m.group(1).upper())
    take, why_not = [], {}
    for name, n in sorted(samples.items(), key=lambda kv: -kv[1]):
        if n < a.min_samples:
            continue
        f = index.funcs.get(name)
        insns = None
        if f:  # the guest instructions: one `\t// <mnemonic>` comment each in the generated function
            body = gen_lines(f['file'])[f['at'][0]:f['at'][1] - 1]
            insns = sum(1 for line in body if re.match(r'^\t// [a-z]', line))
        calls = bool(f and (f.get('callees') or f.get('unknown')))
        reason = None
        if name in listed:
            reason = 'listed'
        elif hooks.replaced(name):
            reason = 'hooked'
        elif (why := ln.fits(index, hooks, name)):
            reason = why
        elif insns is not None and insns <= a.tiny and not calls:
            reason = 'tiny leaf'
        elif insns is not None and insns < a.min_instructions and n < a.small_keep_samples:
            reason = 'small'
        if reason:
            why_not[reason] = why_not.get(reason, 0) + 1
            continue
        take.append((name, n, insns))
    lines = [f'{name[4:]} PC profile ({os.path.basename(os.path.dirname(a.profile)) or a.profile}): {n} samples, '
             f'{insns} instructions' for name, n, insns in take]
    print(f'{len(samples)} guest functions sampled; {len(take)} taken; left out: {why_not}')
    if a.write:
        try:
            ends = open(a.write, 'rb').read()[-1:] in (b'', b'\n')
        except OSError:
            ends = True
        with open(a.write, 'a', encoding='utf-8', newline='\n') as w:
            w.write(('' if ends else '\n') + ''.join(line + '\n' for line in lines))
    else:
        print('\n'.join(lines))


if __name__ == '__main__':
    main()
