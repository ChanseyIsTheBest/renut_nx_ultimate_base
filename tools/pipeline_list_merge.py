#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the GPU goal): one Renut pipeline list (render/pipeline.cpp "RNPL") from several, each record
once (by its bytes), in the order first met. The lists must share the version and key size (the console's and
Ryujinx's of one build do). For tools that take one list (the installer's tools/make_release.py --pipeline-list).

    python tools/pipeline_list_merge.py OUT.bin LIST.bin [LIST.bin ...]

A record is a u32 size and that many bytes (the key, then the layout); a record cut short ends that list's reading,
as tools/deko3d_variant_cache.py read_list does. Game data: none (shader hashes and pipeline state only).
"""
import struct
import sys


def records(path):
    data = open(path, 'rb').read()
    magic, version, key_size, streams = struct.unpack_from('<4sIII', data, 0)
    if magic != b'RNPL':
        sys.exit(f'pipeline_list_merge: {path} is not a pipeline list')
    out = []
    at = 16
    while at + 4 <= len(data):
        (size,) = struct.unpack_from('<I', data, at)
        if at + 4 + size > len(data) or size < key_size:
            break
        out.append(data[at:at + 4 + size])
        at += 4 + size
    return data[:16], (version, key_size, streams), out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out_path, lists = sys.argv[1], sys.argv[2:]
    head, form, seen, merged = None, None, set(), []
    for path in lists:
        h, f, recs = records(path)
        if form is None:
            head, form = h, f
        elif f != form:
            sys.exit(f'pipeline_list_merge: {path} is version/key size/streams {f}, not {form}')
        new = 0
        for r in recs:
            if r not in seen:
                seen.add(r)
                merged.append(r)
                new += 1
        print(f'{path}: {len(recs)} records, {new} new')
    open(out_path, 'wb').write(head + b''.join(merged))
    print(f'{out_path}: {len(merged)} records')


if __name__ == '__main__':
    main()
