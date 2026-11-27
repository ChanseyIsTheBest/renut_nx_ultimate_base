#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, batch 4, row 24; the user: "implement the 10 things"): loop barriers relaxed where a loop
writes guest memory.

tools/loop_barriers.py puts REX_LOOP_BARRIER() at a loop header unless the loop reads no guest memory or every way
round it passes an indirect call (and localize_native.py leaves out a counted loop's): without it GCC may hoist a
guest load out of a polling loop (`while (*flag == 0) {}`) and spin forever. A loop that also writes guest memory is
rarely such a wait; there the barrier only makes the compiler reload guest values every iteration. This step writes
those barriers as REX_LOOP_BARRIER_W(): the barrier itself by default (exact), nothing with REX_SSASR_LOOSE_BARRIERS
(CMake SSASR_NON_EXACT). Not exact: a polling loop that also stores could lose its re-read. Checked by
tools/find_self_loops.py on the Switch ELF (a hoisted poll becomes a branch to itself) and by runs.

A loop is its header label `loc_X:` (followed by the barrier) to the last `goto loc_X;` of the function; it writes
guest memory when a line in that span is a guest store (REX_STORE_*, BANJO_HOT_STORE_*, a vector store to
REX_RAW_ADDR, REX_STV*, a memset / memcpy into guest memory). Run by tools/post_codegen.py after the copies are made
(the generated files and the copies). Idempotent.

  python tools/ssasr_loose_barriers.py [--gen app/generated] [--check]
"""
import argparse
import glob
import os
import re

LABEL = re.compile(r'^(loc_[0-9A-F]+):$')
FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
# Functions whose loops keep the plain barrier: tools/find_self_loops.py on the Switch ELF built with loose barriers
# finds a branch to itself in each (GCC proved a loop could not end once its barrier was gone: a wait on memory another
# thread writes, turned into a spin; a console hang). renut-ultimate: each port's own list, app/config/
# loose_barriers_keep.txt (one sub_ address or hex address a line, # comments); none without it. ssasr-nx's ten:
# 822CD4E8 82362A88 82364868 82386428 8238BAA8 8238BFA8 8238C4C0 8238C798 8238CBB8 823C6008.
KEEP_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app', 'config',
                         'loose_barriers_keep.txt')
KEEP = set()
if os.path.isfile(KEEP_FILE):
    for _line in open(KEEP_FILE, encoding='utf-8'):
        _word = _line.split('#')[0].strip().upper().replace('SUB_', '')
        if _word:
            KEEP.add(_word)
BARRIER = 'REX_LOOP_BARRIER();'
STORE = re.compile(r'REX_STORE_|BANJO_HOT_STORE_|simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR|REX_STV|'
                   r'memset\(\(void\*\)REX_RAW_ADDR|memcpy\(\(void\*\)REX_RAW_ADDR')
MARKER = '// ssasr-nx: loop barriers of loops that write guest memory (tools/ssasr_loose_barriers.py)'
END = '// (end of tools/ssasr_loose_barriers.py)\n'
BLOCK = MARKER + '''
#ifndef REX_LOOP_BARRIER_W
#if defined(REX_SSASR_LOOSE_BARRIERS)
#define REX_LOOP_BARRIER_W() ((void)0)
#else
#define REX_LOOP_BARRIER_W() REX_LOOP_BARRIER()
#endif
#endif
''' + END


def patch_text(lines, func=None):
    """'func': the copy's function (a hot/n_X.h file holds one function's forms); else from DEFINE_REX_FUNC."""
    n = kept = 0
    i = 0
    while i < len(lines):
        f = FUNC.match(lines[i])
        if f:
            func = f.group(1)
        m = LABEL.match(lines[i].strip())
        if not m or func in KEEP:
            if m:
                kept += 1
            i += 1
            continue
        label = m.group(1)
        # the barrier: on one of the next few lines (s.Hit / REX_LOOP_LOCAL may come first)
        b = next((k for k in range(i + 1, min(len(lines), i + 4)) if lines[k].strip() == BARRIER), None)
        if b is None:
            i += 1
            continue
        back = 'goto %s;' % label
        last = None
        for k in range(b + 1, len(lines)):
            if lines[k] == '}':  # the function's end
                break
            if back in lines[k]:
                last = k
        if last is not None and any(STORE.search(lines[k]) for k in range(b + 1, last + 1)):
            ind = lines[b][:len(lines[b]) - len(lines[b].lstrip())]
            lines[b] = ind + 'REX_LOOP_BARRIER_W();'
            n += 1
        else:
            kept += 1
        i += 1
    return n, kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated'))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restore', action='store_true',
                    help='write every REX_LOOP_BARRIER_W back as REX_LOOP_BARRIER (post_codegen.py runs this before '
                         'loop_barriers.py, which knows only the plain form, and before the copies are made)')
    a = ap.parse_args()
    paths = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))) + sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h')))
    if a.restore:
        back = 0
        for path in paths:
            text = open(path, encoding='utf-8').read()
            if 'REX_LOOP_BARRIER_W();' in text:
                back += text.count('REX_LOOP_BARRIER_W();')
                if not a.check:
                    open(path, 'w', encoding='utf-8', newline='\n').write(text.replace('REX_LOOP_BARRIER_W();',
                                                                                       'REX_LOOP_BARRIER();'))
        print(f'ssasr_loose_barriers --restore: {back} barriers back to REX_LOOP_BARRIER')
        return
    for p in glob.glob(os.path.join(a.gen, '*_pch.h')):
        text = open(p, encoding='utf-8').read()
        new = text
        if MARKER in new:
            s = new.index(MARKER)
            new = new[:s] + new[new.index(END, s) + len(END):]
        anchor = '#define REX_LOAD_U8(x)'
        new = new.replace(anchor, BLOCK + anchor, 1) if anchor in new else new.rstrip('\n') + '\n' + BLOCK
        if new != text and not a.check:
            open(p, 'w', encoding='utf-8', newline='\n').write(new)
    total = kept_all = files = 0
    for path in paths:
        lines = open(path, encoding='utf-8').read().split('\n')
        copy = re.match(r'n_([0-9A-F]{8})\.h$', os.path.basename(path))
        n, kept = patch_text(lines, copy.group(1) if copy else None)
        kept_all += kept
        if n:
            files += 1
            total += n
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print(f'ssasr_loose_barriers: {total} loop barriers in loops that write guest memory as REX_LOOP_BARRIER_W '
          f'({files} files); {kept_all} kept{" (check only)" if a.check else ""}')


if __name__ == '__main__':
    main()
