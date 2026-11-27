#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the user: "can we have CPU core thread function breakdown please"): a console run's CPU time
by core, by thread and by function, from the SDK profiler's log (logs/rex/rex_profile.log) over a span of its 10 s
windows, with the link map of the same build naming the sampled code (tools/switch_hot_functions.py's map reader).

    python tools/switch_cpu_breakdown.py rex_profile.log --map app/out/switch/renut.map [--from N] [--to M]
        [--min-fps 0 --max-fps 999] [--top 12]

Windows are numbered from 1 in the order the log has them ("==== 10.0 s | mode: ..."); --from / --to pick a span
(the race), --min-fps / --max-fps keep only windows whose game fps is in range. Per window the log gives the CPU total
(of 400 %: four cores), the system core's idle share, and each thread's CPU %, priority, preferred core and core
mask, with its samples ("pc image+0x... xN", busy) and wait return addresses ("lr ..."). This prints:
  - the windows used (game fps, CPU total, system core 3 idle);
  - per thread: average CPU % (of one core), priority, preferred core / mask, and its share of the busy samples;
  - per core: the threads pinned to it (mask with one bit) and their CPU, the floating threads (mask 0x7: cores 0-2)
    shared out by what is left, and core 3's own idle from the log;
  - per thread: its top functions by self time (busy pc samples), each with its object (game code: app / default =
    recompiled 360 functions, banjo::hot = machine copies; the rest host code: the renderer, the SDK, libnx, Mesa).
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_hot_functions as shf  # noqa: E402

WIN = re.compile(r'^==== ([0-9.]+) s \| mode: (\S+) \| game ([0-9.]+) fps \| CPU total ([0-9]+)%')
CORE3 = re.compile(r'system core 3: idle ([0-9.]+)%')
THREAD = re.compile(r'^-- thread (\d+) "([^"]*)": CPU ([0-9.]+)% \| priority (0x[0-9A-Fa-f]+), preferred core (-?\d+), '
                    r'mask (0x[0-9A-Fa-f]+)')
SAMPLES = re.compile(r'^\s+(\d+) samples, (\d+)% waiting in the kernel')
PC = re.compile(r'^\s+[0-9.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')


def parse(path):
    wins = []
    cur = None
    th = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = WIN.match(line)
        if m:
            cur = {'fps': float(m.group(3)), 'cpu': int(m.group(4)), 'mode': m.group(2), 'core3_idle': None,
                   'threads': {}}
            wins.append(cur)
            th = None
            continue
        if cur is None:
            continue
        m = CORE3.search(line)
        if m and cur['core3_idle'] is None:
            cur['core3_idle'] = float(m.group(1))
        m = THREAD.match(line)
        if m:
            name = f'{m.group(2)} [{m.group(1)}]'
            th = {'cpu': float(m.group(3)), 'prio': m.group(4), 'core': int(m.group(5)), 'mask': m.group(6),
                  'pcs': collections.Counter(), 'samples': 0, 'wait': 0}
            cur['threads'][name] = th
            continue
        if th is None:
            continue
        m = SAMPLES.match(line)
        if m:
            th['samples'] = int(m.group(1))
            th['wait'] = int(m.group(2))
            continue
        m = PC.match(line)
        if m:
            th['pcs'][int(m.group(1), 16)] += int(m.group(2))
    return wins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--map', default=os.path.join(shf.ROOT, 'app', 'out', 'switch', 'renut.map'))
    ap.add_argument('--from', dest='first', type=int, default=1)
    ap.add_argument('--to', dest='last', type=int, default=10 ** 9)
    ap.add_argument('--min-fps', type=float, default=0.0)
    ap.add_argument('--max-fps', type=float, default=1e9)
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    wins = parse(a.log)
    used = [(i + 1, w) for i, w in enumerate(wins)
            if a.first <= i + 1 <= a.last and w['mode'] == 'normal' and a.min_fps <= w['fps'] <= a.max_fps]
    if not used:
        sys.exit('no window in that span')
    n = len(used)
    print(f'{len(wins)} windows in the log; {n} used: ' +
          '; '.join(f'#{i} {w["fps"]:.1f} fps CPU {w["cpu"]}% core3 idle {w["core3_idle"]}%' for i, w in used))
    print(f'average: game {sum(w["fps"] for _, w in used) / n:.1f} fps, CPU total {sum(w["cpu"] for _, w in used) / n:.0f}% '
          f'of 400%, system core 3 idle {sum((w["core3_idle"] or 0) for _, w in used) / n:.0f}%')
    threads = collections.OrderedDict()
    for _, w in used:
        for name, t in w['threads'].items():
            e = threads.setdefault(name, {'cpu': 0.0, 'prio': t['prio'], 'core': t['core'], 'mask': t['mask'],
                                          'pcs': collections.Counter(), 'samples': 0})
            e['cpu'] += t['cpu']
            e['pcs'].update(t['pcs'])
            e['samples'] += sum(t['pcs'].values())
    for e in threads.values():
        e['cpu'] /= n
    busy_total = sum(e['samples'] for e in threads.values()) or 1
    print('\nper thread (CPU % of one core, averaged over the windows; share of the busy samples):')
    for name, e in sorted(threads.items(), key=lambda x: -x[1]['cpu']):
        if e['cpu'] < 0.5:
            continue
        print(f'  {e["cpu"]:5.1f}%  {name:34} prio {e["prio"]}, core {e["core"]:>2}, mask {e["mask"]}  '
              f'({100 * e["samples"] / busy_total:4.1f}% of busy samples)')
    print('\nper core (pinned threads by their mask; the floating ones, mask 0x7, run on cores 0-2):')
    cores = collections.defaultdict(float)
    floating = 0.0
    for name, e in threads.items():
        mask = int(e['mask'], 16)
        if mask and mask & (mask - 1) == 0:
            cores[mask.bit_length() - 1] += e['cpu']
        else:
            floating += e['cpu']
    for c in range(4):
        print(f'  core {c}: pinned threads {cores[c]:5.1f}%')
    print(f'  floating threads (cores 0-2): {floating:5.1f}% (of the {300 - sum(cores[c] for c in range(3)):.0f}% the '
          f'pinned ones leave on cores 0-2)')
    print(f'  CPU total {sum(w["cpu"] for _, w in used) / n:.0f}% of 400%')
    sections = shf.load_map(a.map)
    starts = [s[0] for s in sections]

    def name_of(off):
        i = __import__('bisect').bisect_right(starts, off) - 1
        if i >= 0 and sections[i][0] <= off < sections[i][1]:
            return sections[i][2], sections[i][3]
        return '?', '?'

    demangled = {}

    def demangle_all(names):
        """devkitA64's c++filt (one call for every name), when it is there."""
        filt = r'C:/devkitPro/devkitA64/bin/aarch64-none-elf-c++filt.exe'
        todo = [x for x in names if x.startswith('_Z') and x not in demangled]
        if not todo or not os.path.isfile(filt):
            return
        import subprocess
        out = subprocess.run([filt], input='\n'.join(todo), capture_output=True, text=True).stdout.split('\n')
        for k, v in zip(todo, out):
            demangled[k] = v

    def short(fn):
        m = re.search(r'f_(sub_[0-9A-F]{8})', fn)
        if m and 'hot' in fn:
            return f'{m.group(1)} (machine copy)'
        d = demangled.get(fn, fn)
        d = d.replace('(anonymous namespace)::', '')
        # the name without its parameters
        depth = 0
        for i, c in enumerate(d):
            if c == '<':
                depth += 1
            elif c == '>':
                depth -= 1
            elif c == '(' and depth == 0 and i > 0:
                d = d[:i]
                break
        return d[:80]
    for name, e in sorted(threads.items(), key=lambda x: -x[1]['cpu']):
        if e['cpu'] < 1.0 or not e['samples']:
            continue
        funcs = collections.Counter()
        objs = {}
        for off, cnt in e['pcs'].items():
            fn, obj = name_of(off)
            funcs[fn] += cnt
            objs[fn] = obj
        demangle_all([fn for fn, _ in funcs.most_common(a.top)])
        print(f'\n== {name}: {e["cpu"]:.1f}% of a core, {e["samples"]} busy samples ==')
        for fn, cnt in funcs.most_common(a.top):
            print(f'  {100 * cnt / e["samples"]:5.1f}%  {os.path.basename(objs[fn])[:24]:24} {short(fn)}')


if __name__ == '__main__':
    main()
