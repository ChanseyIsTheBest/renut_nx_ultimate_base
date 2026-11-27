#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): finds the machine copies whose GUARDED calls break the game (the guard run at
banjo_hot_guard_calls=4000, banjo_hot_guard_period=1 shows the autosave notice's text as empty boxes from about 15 s and
never leaves that screen; with banjo_cpu_native_bulk=false it boots normally). Each step is a short PC run
(tools/run_pc.ps1, no input script) with the guard on and a set of copies turned off by their own keys
(banjo_cpu_native_sub_XXXXXXXX=false); the run is "bad" when its captures match the broken screen (a reference
capture, compared at 160x90 in grey: mean absolute difference under --threshold).

  python tools/guard_bisect.py --ref out/pc_runs/verify_race1/captura_002_0030s.png [--seconds 30] [--threshold 12]

Bisects the copies of app/src/cpu/hot/localized_bulk.txt: keeps the half that still breaks the game, until one copy
(or a set that only breaks together) is left. Writes out/guard_bisect.log.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

from PIL import Image, ImageChops, ImageStat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def grey(path):
    return Image.open(path).convert('L').resize((160, 90))


def run(name, off, seconds):
    extra = '--banjo_hot_guard_calls=4000 --banjo_hot_guard_period=1 ' + ' '.join(
        f'--banjo_cpu_native_sub_{a}=false' for a in off)
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                    os.path.join(ROOT, 'tools', 'run_pc.ps1'), '-Seconds', str(seconds), '-Name', name, '-Every', '10',
                    '-Extra', extra], cwd=ROOT, capture_output=True, text=True)
    return sorted(glob.glob(os.path.join(ROOT, 'out', 'pc_runs', name, '*.png')))


def bad(caps, ref, threshold):
    diffs = [ImageStat.Stat(ImageChops.difference(grey(c), ref)).mean[0] for c in caps[1:]]  # the first is too early
    return any(d < threshold for d in diffs), [round(d, 1) for d in diffs]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--threshold', type=float, default=12)
    a = ap.parse_args()
    ref = grey(a.ref)
    copies = [ln.split()[0] for ln in open(os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'localized_bulk.txt'))
              if re.match(r'^[0-9A-F]{8}\s', ln)]
    log = open(os.path.join(ROOT, 'out', 'guard_bisect.log'), 'w', encoding='utf-8')

    def say(t):
        print(t, flush=True)
        log.write(t + '\n')
        log.flush()

    suspects = copies
    step = 0
    while len(suspects) > 1:
        half, rest = suspects[:len(suspects) // 2], suspects[len(suspects) // 2:]
        step += 1
        # only `half` guarded (with the copies outside the suspects off too)
        off = [c for c in copies if c not in half]
        is_bad, diffs = bad(run(f'gb_{step}a', off, a.seconds), ref, a.threshold)
        say(f'step {step}: {len(half)} copies on ({half[0]}..): {"BAD" if is_bad else "good"} {diffs}')
        if is_bad:
            suspects = half
            continue
        off = [c for c in copies if c not in rest]
        is_bad, diffs = bad(run(f'gb_{step}b', off, a.seconds), ref, a.threshold)
        say(f'step {step}: {len(rest)} copies on ({rest[0]}..): {"BAD" if is_bad else "good"} {diffs}')
        if is_bad:
            suspects = rest
            continue
        say(f'neither half alone breaks it: a set of {len(suspects)} breaks only together: {" ".join(suspects)}')
        return
    say(f'the copy: {suspects[0]}' if suspects else 'none')


if __name__ == '__main__':
    sys.exit(main())
