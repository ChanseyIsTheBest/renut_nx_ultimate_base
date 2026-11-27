#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): the 10 s lines that decide the porting prompt's four log-decided keys, the SMAA line, the
CG-13 census and the audio channel census, one row per 10 s window of a PC or Ryujinx run's log (porting prompt step 5,
render-thread-turn-offs.md). Gameplay windows are the user's to mark: pass --from / --to (seconds since the first line)
to sum only those.

  python tools/ssasr_log_keys.py out/pc_runs/<run>/<run>.log [--from 120] [--to 330]
"""
import argparse
import datetime
import re

TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]')
PATTERNS = {
    'inert': re.compile(r'sasrt_gpu_skip_inert_draws (?:\(off\) )?(\d+)'),
    'inherit': re.compile(r'replays with the inherited words as recorded \(ultimate_renut_replay_inherit_same\): (\d+) of (\d+)'),
    'tags': re.compile(r'recording from marks \(banjo_renut_tags\): .*?(\d+) whole checks, (\d+) chunks changed without a mark'),
    'redeem': re.compile(r'banjo_renut_stream_redeem: (\d+) dynamic streams'),
    'smaa': re.compile(r'\[renut\] SMAA \(banjo_renut_smaa.*?\): (\d+) frames'),
    'cg13': re.compile(r'CG-13 \(one flush mode\): per second (\d+) any-mode cold paths and (\d+) of (\d+) FPU-mode regions'),
    'audio': re.compile(r'\[audio\] channels with sound(.*)$'),
    'fps': re.compile(r'(\d+(?:\.\d+)?) fps'),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    a = ap.parse_args()
    start = None
    sums = {'inert': 0, 'inherit': 0, 'inherit_of': 0, 'tags_checks': 0, 'tags_unmarked': 0, 'redeem': 0,
            'smaa': 0, 'cg13_den': 0, 'cg13_cold': 0, 'cg13_windows': 0, 'cg13_hit': 0}
    audio = []
    with open(a.log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            t = datetime.datetime.strptime(m.group(1)[:23], '%Y-%m-%d %H:%M:%S.%f')
            if start is None:
                start = t
            s = (t - start).total_seconds()
            if s < a.t0 or s > a.t1:
                continue
            for key, rx in PATTERNS.items():
                mm = rx.search(line)
                if not mm:
                    continue
                if key == 'inert' and '[sasrt gpu] GPU round' in line:
                    v = int(mm.group(1)); sums['inert'] += v
                    print(f'{s:7.1f} s  skip_inert_draws {v}')
                elif key == 'inherit':
                    sums['inherit'] += int(mm.group(1)); sums['inherit_of'] += int(mm.group(2))
                    print(f'{s:7.1f} s  inherit_same {mm.group(1)} of {mm.group(2)}')
                elif key == 'tags':
                    sums['tags_checks'] += int(mm.group(1)); sums['tags_unmarked'] += int(mm.group(2))
                    print(f'{s:7.1f} s  tags {mm.group(1)} checks, {mm.group(2)} unmarked')
                elif key == 'redeem':
                    sums['redeem'] += int(mm.group(1))
                    print(f'{s:7.1f} s  stream_redeem {mm.group(1)}')
                elif key == 'smaa':
                    sums['smaa'] += int(mm.group(1))
                elif key == 'cg13':
                    sums['cg13_windows'] += 1
                    cold, den = int(mm.group(1)), int(mm.group(2))
                    sums['cg13_cold'] += cold; sums['cg13_den'] += den
                    if cold or den:
                        sums['cg13_hit'] += 1
                    print(f'{s:7.1f} s  CG-13 cold {cold}, denormal regions {den} of {mm.group(3)} a second')
                elif key == 'audio':
                    audio.append(f'{s:7.1f} s  audio{mm.group(1)}')
    for line in audio:
        print(line)
    print('sums:', sums)


if __name__ == '__main__':
    main()
