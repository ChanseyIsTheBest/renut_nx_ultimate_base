#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25, the user: "give me a cpu core thread function breakdown"): a console profile
(logs/rex/rex_profile.log, switch_profile_stacks) by thread id: each thread's average CPU over the gameplay reports, its
core mask and preferred core, its samples by kind (copy / generated / hooked / host) and its top functions, mapped with
the NRO's ELF. tools/profile_threads.py (the base's) keys threads by priority and core mask, so every game "XThread"
falls into one row; this keeps each thread apart.

  python tools/profile_by_thread.py PROFILE ELF [--from-report 2] [--top 12] [--threads 8] [--overall N]

renut-ultimate (the hardening on the way in): a profile with no report after --from-report says so instead of a
division by zero.
"""
import argparse
import bisect
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_hot_bulk as shb  # noqa: E402

THREAD = re.compile(r'^-- thread (\d+) "([^"]*)": CPU ([\d.]+)% \| priority (\S+), preferred core (-?\d+), mask (0x[0-9a-fA-F]+)')
PC = re.compile(r'^\s+[\d.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')
REPORT = re.compile(r'^==== .*\| game ([\d.]+) fps \| CPU total (\d+)%')
CLOCKS = re.compile(r'clocks: CPU ([\d.]+) MHz, GPU ([\d.]+) MHz.*process cores (\d+) \(mask (0x[0-9a-f]+)\)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('elf')
    ap.add_argument('--from-report', type=int, default=2)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--overall', type=int, default=0,
                    help='instead: the top N functions over every thread, each with its share of all samples, an '
                         'estimate of the cores it takes (that share of the run\'s mean CPU) and its threads')
    a = ap.parse_args()
    starts, names, by_name, in_place = shb.symbols(a.elf)
    lines = open(a.profile, encoding='utf-8', errors='replace').read().split('\n')
    report = -1
    fps, total, clocks = [], [], None
    cpu = collections.defaultdict(list)
    info = {}
    samples = collections.defaultdict(collections.Counter)
    current = None
    for l in lines:
        m = REPORT.match(l)
        if m:
            report += 1
            current = None
            if report >= a.from_report:
                fps.append(float(m.group(1)))
                total.append(int(m.group(2)))
            continue
        if report < a.from_report:
            continue
        m = CLOCKS.search(l)
        if m:
            clocks = m.groups()
            continue
        m = THREAD.match(l)
        if m:
            tid = int(m.group(1))
            current = tid
            cpu[tid].append(float(m.group(3)))
            info[tid] = (m.group(2), m.group(5), m.group(6))
            continue
        m = PC.match(l)
        if m and current is not None:
            samples[current][int(m.group(1), 16)] += int(m.group(2))
    n = len(fps)
    if not n:
        sys.exit(f'no 10-s report after the first {a.from_report} in {a.profile} (--from-report)')
    print(f'{n} gameplay reports of 10 s: {sum(fps) / n:.1f} fps on average (min {min(fps):.1f}, max {max(fps):.1f}); '
          f'CPU {sum(total) / n:.0f}% on average (100% = one core)')
    if clocks:
        print(f'clocks: CPU {clocks[0]} MHz, GPU {clocks[1]} MHz; the process has {clocks[2]} cores (mask {clocks[3]})')
    order = sorted(cpu, key=lambda t: -sum(cpu[t]) / n)
    if a.overall:
        # Each thread's samples weighted to its CPU: a thread's samples are taken at one rate while it runs, so its
        # share of the CPU is its share of the run's mean CPU; a function's CPU is its share of its thread's samples
        # times that thread's mean CPU.
        func_cpu = collections.Counter()
        func_kind = {}
        func_threads = collections.defaultdict(collections.Counter)
        for tid, c in samples.items():
            tot = sum(c.values())
            if not tot:
                continue
            thread_cpu = sum(cpu[tid]) / n
            for off, k in c.items():
                i = bisect.bisect_right(starts, off) - 1
                sym = names[i] if i >= 0 else '?'
                kind, what = shb.demangled_kind(sym, by_name, in_place)
                share = thread_cpu * k / tot
                func_cpu[what] += share
                func_kind[what] = kind
                func_threads[what][tid] += share
        all_cpu = sum(func_cpu.values())
        print(f'\ntop {a.overall} functions over every thread ({all_cpu:.0f}% CPU in all, 100% = one core; the % column '
              f'is the share of all of it)')
        print(f'{"#":>3} {"CPU":>6} {"share":>6} {"cum":>6}  {"kind":6} {"function":44} threads (CPU)')
        cum = 0.0
        for rank, (what, c) in enumerate(func_cpu.most_common(a.overall), 1):
            cum += c
            ths = ', '.join(f'{t} {v:.1f}%' for t, v in func_threads[what].most_common(3))
            print(f'{rank:3d} {c:5.1f}% {100 * c / all_cpu:5.1f}% {100 * cum / all_cpu:5.1f}%  {func_kind[what]:6} '
                  f'{what[:44]:44} {ths}')
        return
    print('\nthread                                   CPU (mean of the reports)   core mask / preferred core')
    for tid in order[:20]:
        name, pref, mask = info[tid]
        avg = sum(cpu[tid]) / n
        if avg < 0.5:
            break
        print(f'  {tid:4d} {name[:30]:30s} {avg:6.1f}%  (max {max(cpu[tid]):5.1f}%)       {mask} / {pref}')
    for tid in order[:a.threads]:
        name, pref, mask = info[tid]
        c = samples[tid]
        tot = sum(c.values())
        if not tot:
            continue
        per_func = collections.Counter()
        kinds = collections.Counter()
        for off, k in c.items():
            i = bisect.bisect_right(starts, off) - 1
            sym = names[i] if i >= 0 else '?'
            kind, what = shb.demangled_kind(sym, by_name, in_place)
            per_func[(kind, what)] += k
            kinds[kind] += k
        print(f'\n== thread {tid} "{name}" ({sum(cpu[tid]) / n:.1f}% CPU, mask {mask}): {tot} samples: ' +
              ', '.join(f'{k} {100 * v / tot:.1f}%' for k, v in kinds.most_common()))
        for (kind, what), k in per_func.most_common(a.top):
            print(f'   {100 * k / tot:5.1f}%  {k:5d}  {kind:6s} {what[:100]}')


if __name__ == '__main__':
    main()
