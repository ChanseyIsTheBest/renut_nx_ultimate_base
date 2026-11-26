#!/usr/bin/env python3
"""pokersmash-nx (2026-11-26, the user: "performance analysis"): two console runs side by side, window by window, so a
different scene is not read as a different build. From each run's rex_profile.log (the 10-s reports: mode, game fps,
CPU) and its main log (Renut's 10-s lines: game frames, draw calls, render-thread and GPU time a frame), per run:
the windows by mode, and for the gameplay windows the frame rate, draws a frame, CPU a frame and each thread's CPU
a frame (a thread's CPU in a window / the window's frames: what a frame costs it, whatever the frame rate).

  python tools/console_run_compare.py NAME_A PROFILE_A LOG_A NAME_B PROFILE_B LOG_B [--from-report 2]

Poker Smash's rc5 against rc4: 49.1 fps against 57.0, but 17,275 draw calls a second against 14,041 (a heavier scene,
not a slower build); per unit of work the build matched rc4 but for its memcpy native.

renut-ultimate (the hardening on the way in): Renut's line matched whatever its window's length ("10.1 s" too), a run
with no report says so, and the table shows each of the two runs' six busiest threads with both runs' values (a
thread outside one run's six read as nan there).
"""
import argparse
import collections
import re
import statistics

REPORT = re.compile(r'^==== ([\d.]+) s \| mode: (\w+) \| game ([\d.]+) fps \| CPU total (\d+)%')
THREAD = re.compile(r'^-- thread (\d+) "([^"]*)": CPU ([\d.]+)% \| priority (\S+), preferred core (-?\d+), mask (0x[0-9a-fA-F]+)')
RENUT = re.compile(r'\[renut\] [\d.]+ s: (\d+) game frames \(([\d.]+) fps\), render thread busy ([\d.]+) ms a frame.*?GPU ([\d.]+) ms a frame.*?; (\d+) draw')


def profile(path, skip):
    wins = []
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = REPORT.match(line)
        if m:
            cur = {'mode': m.group(2), 'fps': float(m.group(3)), 'cpu': int(m.group(4)), 'threads': {}}
            wins.append(cur)
            continue
        m = THREAD.match(line)
        if m and cur is not None:
            name = re.sub(r'[0-9A-F]{8}', '', m.group(2)).strip() or '?'
            key = f'{name} {m.group(6)}'
            cur['threads'][key] = cur['threads'].get(key, 0.0) + float(m.group(3))
    return wins[skip:]


def renut(path):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = RENUT.search(line)
        if m:
            out.append({'frames': int(m.group(1)), 'fps': float(m.group(2)), 'render': float(m.group(3)),
                        'gpu': float(m.group(4)), 'draws': int(m.group(5))})
    return out


def summary(name, wins, rl):
    if not wins:
        raise SystemExit(f'{name}: no 10-s report after --from-report')
    modes = collections.Counter(w['mode'] for w in wins)
    play = [w for w in wins if w['mode'] == 'gameplay'] or wins
    print(f'== {name}: {len(wins)} reports; modes {dict(modes)}')
    fps = [w['fps'] for w in play]
    print(f'   gameplay windows: {len(play)}; fps mean {statistics.mean(fps):.1f}, median {statistics.median(fps):.1f}, '
          f'min {min(fps):.1f}, max {max(fps):.1f}; CPU {statistics.mean(w["cpu"] for w in play):.0f} %')
    busy = [r for r in rl if r['frames'] > 300]
    if busy:
        print(f'   Renut (windows over 300 frames: {len(busy)}): draws a frame '
              f'{statistics.mean(r["draws"] / r["frames"] for r in busy):.0f}, render thread '
              f'{statistics.mean(r["render"] for r in busy):.2f} ms a frame, GPU {statistics.mean(r["gpu"] for r in busy):.2f} ms')
    per = collections.defaultdict(list)
    for w in play:
        if w['fps'] <= 0:
            continue
        for k, c in w['threads'].items():
            per[k].append(c / w['fps'])  # % of a core per (frame a second) = core-ms a frame / 10
    means = {k: statistics.mean(v) * 10 for k, v in per.items() if v}
    top = sorted(means, key=lambda k: -means[k])[:6]
    return means, top, statistics.mean(fps)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs=6)
    ap.add_argument('--from-report', type=int, default=2)
    a = ap.parse_args()
    res = []
    for i in (0, 3):
        name, prof, log = a.runs[i:i + 3]
        res.append((name, *summary(name, profile(prof, a.from_report), renut(log))))
    print('\nthread CPU a frame in gameplay (ms of one core per frame; the frame rate taken out):')
    keys = list(dict.fromkeys(list(res[0][2]) + list(res[1][2])))
    print(f'   {"thread":34} {res[0][0]:>10} {res[1][0]:>10}')
    for k in keys:
        print(f'   {k[:34]:34} {res[0][1].get(k, float("nan")):10.2f} {res[1][1].get(k, float("nan")):10.2f}')


if __name__ == '__main__':
    main()
