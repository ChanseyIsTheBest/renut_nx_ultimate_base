#!/usr/bin/env python3
"""ilomilo-nx (2026-11-25): an emulator or PC test toml from one of the package's tomls, with the run's keys set in
place (tools/make_katamari_tomls.py's apply, a port's own toml generator's in a port: a key the file has is replaced, not added again: TOML refuses a duplicate
key, and the NRO then ignores the whole file, as the first emulator run found). Checked with tomllib before writing.

  python tools/make_emu_toml.py --base dist/switch/katamari-nx/configs/renut_verify.toml --out out/emu_tomls/emu_verify.toml
         [--script "15:START,22:A,..."] [--sticks 110] [--captures 15] [--set key=value ...]
"""
import argparse
import os
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_katamari_tomls import apply  # noqa: E402

SCRIPT = '15:START,22:A,27:A,32:A,37:A,42:A,47:A,52:A,57:A,62:A,67:A,72:A,80:A,85:A,90:A,95:A,100:A'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--script', default=SCRIPT)
    ap.add_argument('--sticks', type=int, default=110)
    ap.add_argument('--captures', type=int, default=15)
    ap.add_argument('--set', action='append', default=[], help='key=value (TOML value as written)')
    a = ap.parse_args()
    text = open(a.base, encoding='utf-8').read()
    keys = {'nfsmw_captura_cada_s': str(a.captures), 'nfsmw_captura_max': '60',
            'input_script': f'"{a.script}"', 'input_script_sticks': str(a.sticks),
            'banjo_stall_watch_seconds': '0', 'log_level': '"info"', 'log_file_writer': 'true'}
    for kv in a.set:
        k, v = kv.split('=', 1)
        keys[k.strip()] = v.strip()
    text = apply(text, keys)
    text = (f'# test toml (tools/make_emu_toml.py from {os.path.basename(a.base)}): captures every '
            f'{a.captures} s, the scripted presses, the sticks from {a.sticks} s, no stall watchdog (Ryujinx hangs with '
            'it).\n' + text)
    tomllib.loads(text)  # refuses a duplicate key or a bad value before the NRO silently would
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(f'{a.out}: {len(keys)} keys set, valid TOML')


if __name__ == '__main__':
    main()
