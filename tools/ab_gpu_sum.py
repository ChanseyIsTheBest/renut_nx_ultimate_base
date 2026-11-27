#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the GPU goal): the in-run A/B's GPU time per target from a renut log's "[renut A/B]" lines
(banjo_renut_ab_targets; render/... banjo_renut_tuning.cpp writes one a target every 10 s with that window's halves).

    python tools/ab_gpu_sum.py LOG [LOG ...] [--min-gpu-ms 20]

Per target: the GPU ms a frame of the "on" and "off" halves, frame-weighted over the windows where both halves are
at or above --min-gpu-ms (the race, not the menus or loads), the median of the windows' differences (on - off) and how
many windows; negative means the item saves GPU time. Windows catch different parts of the track, so one window says
little; the median over many does.
"""
import argparse
import re
import statistics

PAT = re.compile(r'\[renut A/B\] (.*?) \(target (\d+)[^)]*\): on ([0-9.]+) fps, render thread [0-9.]+ ms a frame, '
                 r'GPU ([0-9.]+) ms a frame \(([0-9.]+) s\).*?; off ([0-9.]+) fps, render thread [0-9.]+ ms a frame, '
                 r'GPU ([0-9.]+) ms a frame \(([0-9.]+) s\)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-gpu-ms', type=float, default=20.0)
    a = ap.parse_args()
    acc = {}
    for path in a.logs:
        for line in open(path, encoding='utf-8', errors='replace'):
            m = PAT.search(line)
            if not m:
                continue
            name, t = m.group(1), int(m.group(2))
            fps_on, gpu_on, s_on, fps_off, gpu_off, s_off = map(float, m.groups()[2:])
            if gpu_on < a.min_gpu_ms or gpu_off < a.min_gpu_ms or not s_on or not s_off:
                continue
            e = acc.setdefault(t, {'name': name, 'on': [0.0, 0.0], 'off': [0.0, 0.0], 'diff': []})
            e['on'][0] += gpu_on * fps_on * s_on
            e['on'][1] += fps_on * s_on
            e['off'][0] += gpu_off * fps_off * s_off
            e['off'][1] += fps_off * s_off
            e['diff'].append(gpu_on - gpu_off)
    if not acc:
        print('no [renut A/B] window with both halves at or above', a.min_gpu_ms, 'ms')
    for t, e in sorted(acc.items()):
        on = e['on'][0] / e['on'][1] if e['on'][1] else 0.0
        off = e['off'][0] / e['off'][1] if e['off'][1] else 0.0
        med = statistics.median(e['diff']) if e['diff'] else 0.0
        print(f'target {t:3} {e["name"][:48]:48}  on {on:6.2f}  off {off:6.2f} ms a frame  (on - off {on - off:+.2f}; '
              f'median of {len(e["diff"])} windows {med:+.2f})')


if __name__ == '__main__':
    main()
