#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25, the user: "are you sure they are waiting on something"): every sampled instruction of a
function in a console profile, by what it is, so a "waiting on memory" reading rests on all of its samples and not on
its hottest spots alone. Per function: the share of its samples on loads (ldr/ldp/ld1 ...), on atomics (ldxr/stxr/ldaxr/
cas/ldadd ...), on stores, on floating-point / vector arithmetic, on integer arithmetic and branches; and how much of the
load share sits on the instruction right after a load as well (a sample usually lands on the instruction after the slow
one, so a stalled load shows on itself or on its first use). Needs the ELF of the same build and devkitA64's binutils.

  python tools/hot_pc_classes.py PROFILE ELF "thread substring" FUNC [FUNC ...] [--from-report 2] [--hot N]
                                [--binutils PREFIX]

Poker Smash's top 15 (its notes, "The hand track"): a first reading by the hottest spots alone called four of them
flat that were half the XDK memcpy inlined; by every sample they were 38-85 % loads, and the memcpy native was the one
rewrite the samples supported (sub_821E4C60 -52 % per unit of work in the next console run).

renut-ultimate (the hardening on the way in): devkitA64's binutils from --binutils (the "aarch64-none-elf-" prefix),
else $DEVKITPRO, else C:/devkitPro; an ELF with no function symbol, or binutils that do not run, say so.
"""
import argparse
import collections
import os
import re
import subprocess
import sys


def binutils_prefix(given):
    """The aarch64 binutils' path prefix and the executables' suffix."""
    suffix = '.exe' if os.name == 'nt' else ''
    if given:
        return given, ('' if given.endswith('.exe') else suffix)
    root = os.environ.get('DEVKITPRO') or 'C:/devkitPro'
    if os.name == 'nt' and root.startswith('/'):  # an msys2 path seen from a Windows shell
        root = 'C:/devkitPro'
    return os.path.join(root, 'devkitA64', 'bin', 'aarch64-none-elf-'), suffix
THREAD = re.compile(r'^-- thread \d+ "([^"]*)"')
PC = re.compile(r'^\s+[\d.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')
REPORT = re.compile(r'^==== ')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$')

ATOMIC = re.compile(r'^(ldx|stx|ldax|stlx|cas|ldadd|ldclr|ldeor|ldset|swp|ldar|stlr)')
LOAD = re.compile(r'^(ldr|ldp|ldur|ld1|ld2|ld3|ld4|ldnp|ldtr|prfm)')
STORE = re.compile(r'^(str|stp|stur|st1|st2|st3|st4|stnp)')
FP = re.compile(r'^(f|s?cvt|u?cvt|frint|fcvt|scvtf|ucvtf|fmov|dup|ins|uzp|zip|trn|ext|rev64|tbl|mul|mla|fmla|fmul|'
                r'fadd|fsub|fdiv|fsqrt|fmax|fmin|fabs|fneg|fcmp|fccmp|fcsel|movi|mvni|orr\s+v|and\s+v|bic\s+v|eor\s+v)')


def classify(mnem, ops):
    if ATOMIC.match(mnem):
        return 'atomic'
    if LOAD.match(mnem):
        return 'load'
    if STORE.match(mnem):
        return 'store'
    if FP.match(mnem) or ops.startswith('v') or re.match(r'^[dsqh]\d', ops):
        return 'fp/vector'
    if mnem.startswith('b') or mnem in ('cbz', 'cbnz', 'tbz', 'tbnz', 'ret', 'br', 'blr'):
        return 'branch/call'
    return 'integer'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('elf')
    ap.add_argument('thread')
    ap.add_argument('funcs', nargs='+')
    ap.add_argument('--from-report', type=int, default=2)
    ap.add_argument('--hot', type=int, default=0, help='also list each function\'s N most sampled instructions, each '
                                                       'with the two before it')
    ap.add_argument('--binutils', default='', help='the aarch64 binutils prefix (".../aarch64-none-elf-")')
    a = ap.parse_args()
    prefix, exe = binutils_prefix(a.binutils)
    try:
        out = subprocess.run([prefix + 'nm' + exe, '-n', '-S', '--defined-only', a.elf], capture_output=True,
                             text=True, errors='replace').stdout
    except OSError as e:
        sys.exit(f'nm did not run ({e}): --binutils names the aarch64-none-elf- prefix')
    syms = []
    for line in out.splitlines():
        p = line.split(' ', 3)
        if len(p) == 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    if not syms:
        sys.exit(f'no function symbol in {a.elf}')
    pcs = collections.Counter()
    report, in_thread = -1, False
    for line in open(a.profile, encoding='utf-8', errors='replace'):
        if REPORT.match(line):
            report += 1
            in_thread = False
            continue
        if report < a.from_report:
            continue
        m = THREAD.match(line)
        if m:
            in_thread = a.thread in m.group(1)
            continue
        m = PC.match(line)
        if m and in_thread:
            pcs[int(m.group(1), 16)] += int(m.group(2))
    print(f'{"function":14} {"samples":>7}  {"load":>6} {"(+use)":>6} {"atomic":>6} {"store":>6} {"fp/vec":>6} '
          f'{"int":>6} {"branch":>6}   hottest load')
    for f in a.funcs:
        # every symbol whose name holds the function's id: its copy (n_<id>::f_sub_<id>, maybe .isra) or itself
        rng = [(s, z, n) for s, z, n in syms if f.replace('sub_', '') in n and z > 0]
        if not rng:
            print(f'{f:14} not in the ELF')
            continue
        s, z, n = max(rng, key=lambda r: sum(v for pc, v in pcs.items() if r[0] <= pc < r[0] + r[1]))
        dis = subprocess.run([prefix + 'objdump' + exe, '-d', '--no-show-raw-insn', f'--start-address=0x{s:x}',
                              f'--stop-address=0x{s + z:x}', a.elf], capture_output=True, text=True,
                             errors='replace').stdout
        insns = {}
        order = []
        for line in dis.splitlines():
            m = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', line)
            if m:
                addr = int(m.group(1), 16)
                insns[addr] = (m.group(2), m.group(3))
                order.append(addr)
        prev = {order[i]: order[i - 1] for i in range(1, len(order))}
        cls = collections.Counter()
        after_load = 0
        hot_load = collections.Counter()
        total = 0
        for pc, v in pcs.items():
            if not (s <= pc < s + z) or pc not in insns:
                continue
            mnem, ops = insns[pc]
            c = classify(mnem, ops)
            cls[c] += v
            total += v
            if c == 'load':
                hot_load[f'{mnem} {ops}'[:34]] += v
            elif pc in prev and classify(*insns[prev[pc]]) == 'load' and c != 'atomic':
                after_load += v
        if not total:
            print(f'{f:14} no samples on {a.thread}')
            continue
        pct = lambda k: f'{100 * cls[k] / total:5.1f}%'
        top = hot_load.most_common(1)[0] if hot_load else ('', 0)
        print(f'{f:14} {total:7d}  {pct("load")} {100 * after_load / total:5.1f}% {pct("atomic")} {pct("store")} '
              f'{pct("fp/vector")} {pct("integer")} {pct("branch/call")}   {top[0]} ({top[1]})')
        if a.hot:
            index = {addr: i for i, addr in enumerate(order)}
            hot = sorted(((v, pc) for pc, v in pcs.items() if s <= pc < s + z and pc in insns), reverse=True)
            for v, pc in hot[:a.hot]:
                i = index[pc]
                ctx = ' | '.join(' '.join(insns[order[j]]) for j in range(max(0, i - 2), i))
                print(f'      {100 * v / total:5.1f}%  +{pc - s:5x}  {" ".join(insns[pc]):40s} after: {ctx}')


if __name__ == '__main__':
    main()
