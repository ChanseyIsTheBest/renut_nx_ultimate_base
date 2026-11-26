#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25): finds every `goto loc_X` in the generated code whose function defines no `loc_X:` (the
compile error "use of undeclared label"), before a build meets them one file at a time. Each comes from block discovery
running past a function's real end: a call that never returns (_CxxThrowException) followed by data, or a switch read
past its table. Prints the function, the label and the guest address of the function's last instruction before it.

  python -I tools/undeclared_labels.py [app/generated]

Poker Smash's first PC build (2026-11-25): a byte table read past its 16 entries (sub_824C4A80) and three MSVC unwind
funclets decoded past their _CxxThrowException call (a [[switch_tables]] entry and three function ends fixed them);
this lists them all at once instead of one compile error per build.

renut-ultimate (the hardening on the way in): any generated function's name (not only sub_X), and exit status 1 when
there is one (a script can stop before the build).
"""
import glob
import os
import re
import sys

DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
LAB = re.compile(r'^(loc_[0-9A-F]+):')
GOTO = re.compile(r'goto (loc_[0-9A-F]+);')


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                            'app', 'generated')
    bad = 0
    for p in sorted(glob.glob(os.path.join(gen, '*.cpp'))):
        fn, labels, gotos = None, set(), []
        lines = open(p, encoding='utf-8', errors='replace').read().split('\n')

        def flush():
            nonlocal bad
            for lab, ln in gotos:
                if lab not in labels:
                    bad += 1
                    print(f'{os.path.basename(p)}:{ln} {fn} {lab}')
        for i, line in enumerate(lines, 1):
            m = DEF.match(line)
            if m:
                if fn:
                    flush()
                fn, labels, gotos = m.group(1), set(), []
                continue
            m = LAB.match(line.strip())
            if m:
                labels.add(m.group(1))
            for g in GOTO.findall(line):
                gotos.append((g, i))
        if fn:
            flush()
    print(f'{bad} gotos to undeclared labels')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
