#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the GPU goal; sasrt-nx's drive_quiet of docs/console-remote-guide.md, credit the SASRT port):
hold ZR (the race's accelerator: the 360's right trigger) for S seconds on the console, re-pressed every 10 s (a press
lost to a pause or a menu is taken again), with at most one screenshot at --shot-at seconds (pixelPeek stalls the game
~250 ms: none inside the measured windows). Steers nothing. Refuses when the running title is not --title (0 to skip
the check: the NRO launched from the Album runs under the Album's id).

    python tools/console_drive.py OUT_DIR SECONDS [--title HEX] [--shot-at 20] [--stick-x 0.0]

Only sys-botbase presses and one pixelPeek; nothing on the SD card.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_remote as sr  # noqa: E402


def title_of(bot):
    words = bot.ask('getTitleID', timeout=8).split()
    try:
        return int(words[-1], 16) if words else 0
    except ValueError:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('seconds', type=float)
    ap.add_argument('--title', default='0')
    ap.add_argument('--shot-at', type=float, default=20.0)
    ap.add_argument('--stick-x', type=float, default=0.0, help='a steady left-stick x in -1..1 (0: none)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    host = sr.host_of(sys.argv)
    bot = sr.Bot(host)
    want = int(a.title, 16)
    now = title_of(bot)
    print(f'running title {now:016X}', flush=True)
    if want and now != want:
        sys.exit(f'the running title is not {want:016X}: nothing pressed')
    t0 = time.time()
    shot_done = False
    try:
        if a.stick_x:
            bot.send(f'setStick LEFT {int(max(-1.0, min(1.0, a.stick_x)) * 32767)} 0')
        while time.time() - t0 < a.seconds:
            bot.send('press ZR')
            t = time.time() - t0
            if not shot_done and t >= a.shot_at:
                n = bot.shot(os.path.join(a.out, f'drive_{int(t):03d}s.jpg'))
                print(f'{t:.0f} s: shot ({n} bytes)', flush=True)
                shot_done = True
            time.sleep(min(10.0, max(0.5, a.seconds - (time.time() - t0))))
    finally:
        bot.send('release ZR')
        if a.stick_x:
            bot.send('setStick LEFT 0 0')
        bot.close()
    print(f'drove {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
