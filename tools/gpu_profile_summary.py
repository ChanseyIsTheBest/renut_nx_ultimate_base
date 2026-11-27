#!/usr/bin/env python3
"""ssasr-nx (2026-11-25, the user: "give me a gpu performance breakdown"): Renut's GPU profile blocks of a console (or
emulator) log ("GPU profile: X ms per frame over N frames" and the lines under it, renut_gpu_profile) summed up:

  - the timeline: each block's time, GPU ms a frame and frames, with the 10 s reports' fps beside them;
  - for the blocks at or above --min-ms (the heavy part of play): every item's frame-weighted average ms a frame, the
    items grouped (scene draws by target size, shadow maps (no pixel shader), resolves, depth resolves, SMAA, present),
    and the heaviest pixel shaders with their draws a frame.

  python tools/gpu_profile_summary.py LOG [LOG ...] [--min-ms 20] [--top 25]
"""
import argparse
import collections
import re

HEAD = re.compile(r'^\[(\S+ \S+)\].*GPU profile: ([0-9.]+) ms per frame over (\d+) frames')
ITEM = re.compile(r'^\s+([0-9.]+) ms\s+(.*?)\s*$')
DRAWS = re.compile(r'^draws (\d+x\d+) (\S+) \| (.*?)\s+\(([0-9.]+) draws a frame\)$')
REPORT = re.compile(r'^\[(\S+ \S+)\].*\[renut\] [0-9.]+ s: (\d+) game frames \(([0-9.]+) fps\), render thread busy '
                    r'([0-9.]+) ms a frame.*?GPU ([0-9.]+) ms a frame')


def group(item):
    m = DRAWS.match(item)
    if not m:
        if item.startswith('SMAA'):
            return 'SMAA (edges, weights, blend)'
        return item
    size, kind, shader = m.group(1), m.group(2), m.group(3)
    if shader.startswith('no pixel shader'):
        return f'depth-only draws {size} (shadow maps / depth pre-pass)'
    if size == '1280x720' or size == '1280x736':
        return f'scene and full-screen draws {size}'
    return f'draws {size} (post-processing / off-screen)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--min-ms', type=float, default=20.0)
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    for path in a.logs:
        blocks = []  # (time, ms, frames, {item: ms})
        reports = []
        cur = None
        for line in open(path, encoding='utf-8', errors='replace'):
            m = HEAD.match(line)
            if m:
                cur = (m.group(1)[11:19], float(m.group(2)), int(m.group(3)), {})
                blocks.append(cur)
                continue
            m = REPORT.match(line)
            if m:
                reports.append((m.group(1)[11:19], float(m.group(3)), float(m.group(4)), float(m.group(5))))
                cur = None
                continue
            if cur is not None:
                m = ITEM.match(line)
                if m:
                    cur[3][m.group(2)] = cur[3].get(m.group(2), 0.0) + float(m.group(1))
                elif line.strip() and not line.startswith(' '):
                    cur = None
        print(f'== {path}: {len(blocks)} GPU profile blocks, {len(reports)} 10 s reports')
        print('10 s reports (time, fps, render thread ms a frame, GPU ms a frame):')
        print('  ' + '; '.join(f'{t} {fps:.1f} fps, {rt:.1f} / {g:.1f} ms' for t, fps, rt, g in reports))
        print('GPU profile blocks (time, GPU ms a frame, frames):')
        print('  ' + '; '.join(f'{t} {ms:.1f} ({n})' for t, ms, n, _ in blocks))
        heavy = [b for b in blocks if b[1] >= a.min_ms]
        if not heavy:
            print(f'no block at or above {a.min_ms} ms')
            continue
        frames = sum(b[2] for b in heavy)
        total = sum(b[1] * b[2] for b in heavy) / frames
        items = collections.Counter()
        draws = collections.Counter()
        for _, _, n, its in heavy:
            for k, v in its.items():
                m = re.match(r'^(.*?)\s+\(([0-9.]+) draws a frame\)$', k)
                key = m.group(1) if m else k
                items[key] += v * n
                if m:
                    draws[key] += float(m.group(2)) * n
        groups = collections.Counter()
        for k, v in items.items():
            groups[group(k + ' (1 draws a frame)' if k.startswith('draws ') else k)] += v
        print(f'the {len(heavy)} blocks at or above {a.min_ms} ms ({frames} frames): {total:.1f} ms a frame on average')
        listed = sum(groups.values()) / frames
        print(f'  by group ({listed:.1f} ms of it listed):')
        for k, v in groups.most_common():
            print(f'    {v / frames:7.2f} ms  {100 * v / frames / total:5.1f} %  {k}')
        print(f'  the heaviest {a.top} items:')
        for k, v in items.most_common(a.top):
            d = f'  ({draws[k] / frames:.1f} draws a frame)' if k in draws else ''
            print(f'    {v / frames:7.2f} ms  {k}{d}')


if __name__ == '__main__':
    main()
