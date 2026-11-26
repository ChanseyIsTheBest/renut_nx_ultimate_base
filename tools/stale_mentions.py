#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25): "Stale mentions" (docs/optimising-functions.md, "What to do next" 3: "Count them
first"): the functions of this game that tools/localize_native.py's Hooks takes as hooked because some app or SDK
source's code (comments left out) names their address, by the file that names them. A file a port does not build
(N&B's, Katamari's) that names another game's address blocks a copy of an unrelated function there.

  python tools/stale_mentions.py [--list FILE]

renut-ultimate (the hardening on the way in): the files and rules are Hooks' own (the three tomls with their comment
rule, app/src without app/src/cpu/hot, sdk/src and sdk/include; comments, Windows-only branches and __imp__ names left
out; 83xxxxxx addresses too when the game's code has them), the generated table by the project's own name, and a total
that differs from Hooks' says so (the two would have drifted apart).
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import localize_native as ln  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sources():
    """Hooks.__init__'s paths: (path, is_toml)."""
    tomls = [os.path.join(ln.APP, 'config', 'renut_hooks.toml'),
             os.path.join(ln.APP, 'renut_config.toml'), os.path.join(ln.APP, 'renut_manifest.toml')]
    paths = [(p, True) for p in tomls if os.path.isfile(p)]
    hot = os.path.abspath(os.path.join(ln.APP, 'src', 'cpu', 'hot'))
    for root in (os.path.join(ln.APP, 'src'), os.path.join(ROOT, 'sdk', 'src'), os.path.join(ROOT, 'sdk', 'include')):
        for d, _, fs in os.walk(root):
            if os.path.abspath(d).startswith(hot):
                continue
            for fn in fs:
                if fn.endswith(('.cpp', '.h', '.inc', '.hpp', '.c')):
                    paths.append((os.path.join(d, fn), False))
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', help='print the blocked addresses this file names (its path as printed)')
    a = ap.parse_args()
    os.chdir(ROOT)
    hooks = ln.Hooks([])
    table = open(ln.gen_file(ln.GEN, 'init.cpp'), encoding='utf-8', errors='replace').read()
    funcs = set(re.findall(r'\{ 0x([0-9A-F]{8}), sub_', table))
    wide = any(f.startswith('83') for f in funcs)  # port 144: as Hooks
    addr_re = (r'(?<![0-9A-Fa-f])(8[23][0-9A-Fa-f]{6})(?![0-9A-Fa-f])' if wide else
               r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
    blocked = {x for x in hooks.addresses if x in funcs}
    by_file = collections.defaultdict(set)
    seen = set()
    for path, toml in sources():
        try:
            text = open(path, encoding='utf-8', errors='ignore').read()
        except OSError:
            continue
        text = ln.strip_comments(text, toml)
        if not toml:
            text = ln.strip_windows_only(text)
        text = re.sub(r'__imp__\w+', ' ', text)
        for x in set(m.upper() for m in re.findall(addr_re, text)):
            seen.add(x)
            if x in blocked:
                by_file[os.path.relpath(path, ROOT).replace(os.sep, '/')].add(x)
    print(f'{len(hooks.addresses)} addresses named in code; {len(blocked)} are functions of this game')
    if seen != hooks.addresses:
        print(f'WARNING: this tool found {len(seen)} addresses, Hooks {len(hooks.addresses)}: its rules and Hooks\' '
              f'have drifted apart (tools/localize_native.py, class Hooks)')
    for f, s in sorted(by_file.items(), key=lambda kv: -len(kv[1])):
        print(f'{len(s):5} {f}')
    if a.list:
        print(' '.join(sorted(by_file.get(a.list.replace(os.sep, '/'), ()))))


if __name__ == '__main__':
    main()
