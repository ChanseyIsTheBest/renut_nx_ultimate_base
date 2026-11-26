#!/usr/bin/env python3
"""ilomilo-nx (2026-11-25): keep the build from recompiling files a tool rewrote with the same bytes.

    python tools/keep_mtimes.py save app/generated out/gen_mtimes.json      (before tools/post_codegen.py)
    python tools/keep_mtimes.py restore app/generated out/gen_mtimes.json   (after it)

save records each file's size, SHA-1 and mtime under the folder; restore puts the recorded mtime back on every file
whose bytes are unchanged, so Ninja rebuilds only what really changed (here: the machine-copy shards after a change
of app/src/cpu/hot/localized_bulk.txt, instead of the whole generated code, about 30 minutes on the Switch).
"""
import hashlib
import json
import os
import sys


def walk(root):
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            yield os.path.relpath(p, root).replace('\\', '/'), p


def digest(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    mode, root, db = sys.argv[1:4]
    if mode == 'save':
        out = {rel: [os.path.getsize(p), digest(p), os.stat(p).st_mtime_ns] for rel, p in walk(root)}
        with open(db, 'w', encoding='utf-8') as f:
            json.dump(out, f)
        print(f'{len(out)} files recorded')
        return
    saved = json.load(open(db, encoding='utf-8'))
    kept = changed = new = 0
    changed_names = []
    for rel, p in walk(root):
        rec = saved.get(rel)
        if rec is None:
            new += 1
            changed_names.append(rel + ' (new)')
            continue
        size, sha, mtime = rec
        if os.path.getsize(p) == size and digest(p) == sha:
            if os.stat(p).st_mtime_ns != mtime:
                os.utime(p, ns=(mtime, mtime))
                kept += 1
        else:
            changed += 1
            changed_names.append(rel)
    print(f'{kept} unchanged files given their old time back; {changed} changed, {new} new')
    for n in changed_names[:60]:
        print('  ' + n)


if __name__ == '__main__':
    main()
