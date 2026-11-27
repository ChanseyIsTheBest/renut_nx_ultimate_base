#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, batch 4 row 11): the redundancy census's hook in every copy. Puts
    REX_SSASR_MEMO_NOTE(0x<address>);
after REX_FUNC_PROLOGUE() at the top of each copy's running form (f_sub_X; the guard's reference o_sub_X is left
alone). Nothing unless REX_SSASR_MEMO_CENSUS (CMake SSASR_MEMO_CENSUS, a PC census build): there it counts the calls
whose argument registers repeat one of the copy's last eight sets (cpu/banjo_hot_common.h MemoNote; the report is
"[census] ssasr memo"). Run by tools/post_codegen.py after the copies are made. Idempotent.

  python tools/ssasr_memo_census.py [--hot app/generated/hot] [--check]
"""
import argparse
import glob
import os
import re

HEAD = re.compile(r'^(\[\[[\w:, ]+\]\] )?static void f_sub_([0-9A-F]{8})\(PPCContext& __restrict ctx, uint8_t\* base, '
                  r'\[\[maybe_unused\]\] S& s\) \{$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hot', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated', 'hot'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    added = files = 0
    for path in sorted(glob.glob(os.path.join(a.hot, 'n_*.h'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        changed = False
        i = 0
        while i < len(lines):
            m = HEAD.match(lines[i])
            if not m:
                i += 1
                continue
            addr = m.group(2)
            for k in range(i + 1, min(len(lines), i + 60)):
                if lines[k] == '}':
                    break
                if lines[k].strip() == 'REX_FUNC_PROLOGUE();':
                    note = '\tREX_SSASR_MEMO_NOTE(0x%sU);' % addr
                    if lines[k + 1] != note:
                        lines.insert(k + 1, note)
                        added += 1
                        changed = True
                    break
            i += 1
        if changed:
            files += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print(f'ssasr_memo_census: {added} copies given REX_SSASR_MEMO_NOTE ({files} files){" (check only)" if a.check else ""}')


if __name__ == '__main__':
    main()
