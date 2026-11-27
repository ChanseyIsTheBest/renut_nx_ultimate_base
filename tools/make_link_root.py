#!/usr/bin/env python3
"""ssasr-nx (2026-11-25): a test game folder made of hard links to the game folder's files (no copy: the same volume, no
extra disk), with the stand-ins of another folder (NAME.xmop, NAME.lz4b, ... as the web installer writes them) in place
of the files they stand for. For a PC run of the installer's re-encoded files (tools/run_pc.ps1 -Game <out>) without
copying the 4 GB game folder.

Never open a file of the result for writing: a hard link IS the game's file. Deleting the folder (its links) leaves the
game folder as it was.

  python tools/make_link_root.py <game folder> <stand-ins folder> <out folder>
"""
import os
import sys

SUFFIXES = ('.xmop.lz4b', '.xmop', '.xmo.lz4b', '.xmo', '.opcm', '.hevc.lz4b', '.hevc', '.lz4b')


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    game, stand, out = (os.path.abspath(p) for p in sys.argv[1:])
    if os.path.exists(out):
        print(f'{out} exists: give a new folder')
        return 1
    # the files the stand-ins replace (relative, with /, lower case as the port's lookups are)
    replaced = {}
    for d, _, names in os.walk(stand):
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), stand).replace('\\', '/')
            for s in SUFFIXES:
                if rel.lower().endswith(s):
                    replaced[rel[:-len(s)].lower()] = rel
                    break
    links = 0
    left_out = 0
    for d, _, names in os.walk(game):
        # every folder, empty ones too (Resource/AI: a scan of the copy must list what the game folder lists)
        os.makedirs(os.path.join(out, os.path.relpath(d, game)), exist_ok=True)
        for n in names:
            src = os.path.join(d, n)
            rel = os.path.relpath(src, game).replace('\\', '/')
            if rel.lower() in replaced:
                left_out += 1
                continue
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.link(src, dst)
            links += 1
    for rel in replaced.values():
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.link(os.path.join(stand, rel), dst)
    print(f'{out}: {links} links to the game, {len(replaced)} stand-ins in place of {left_out} files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
