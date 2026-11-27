#!/usr/bin/env python3
"""ssasr-nx (2026-11-25, the GPU goal): NAME.idx for a DKSH cache made on the PC, as the console's LoadCache writes it
(app/src/renut_engine/gpu/banjo_renut_deko3d.cpp WriteCacheIndex), so the console's first start with a new cache reads
the index instead of walking the whole cache (a 468 MB walk on the SD card at boot).

    python tools/deko3d_cache_index.py out/dksh2/deko3d_shaders.bin [--out out/dksh2/deko3d_shaders.bin.idx] [--check]

The cache: "BDKC", a word, the 16-byte compiler tag, then records (u64 key, u32 size, the DKSH). The index: a head
(magic "BDKI", version 1, the tag, the cache's end of whole records, the count, the first and the last record's keys,
DKSH offsets and sizes) and 16 bytes a record (the key; the DKSH's offset in the low 40 bits and its size in the high
24), sorted by offset. A key met twice keeps its last record, as the console's map does. --check reads an index back
and compares it with the cache as ReadCacheIndex does. Game data: the build tree only.
"""
import argparse
import struct
import sys

TAG = b'uam110-sw8-bnx4\0'  # banjo_renut_deko3d.cpp kCacheTag
HEAD = struct.Struct('<4sI16sQQQQQQII')
REC = struct.Struct('<QQ')
AT_MASK = (1 << 40) - 1


def walk(data):
    """{key: (dksh_offset, size)} and the end of the last whole record, as WalkCacheRecords reads them."""
    if data[:4] != b'BDKC' or data[8:24] != TAG:
        sys.exit('deko3d_cache_index: not a DKSH cache of this compiler (magic or tag)')
    index = {}
    at = 24
    while at + 16 <= len(data):
        key, size = struct.unpack_from('<QI', data, at)
        if size < 64 or size > (16 << 20) or at + 12 + size > len(data) or data[at + 12:at + 16] != b'DKSH':
            break
        index[key] = (at + 12, size)
        at += 12 + size
    return index, at


def make_index(index, end):
    records = sorted(((k, o | (s << 40)) for k, (o, s) in index.items()), key=lambda r: r[1] & AT_MASK)
    first, last = records[0], records[-1]
    head = HEAD.pack(b'BDKI', 1, TAG, end, len(records), first[0], last[0], first[1] & AT_MASK, last[1] & AT_MASK,
                     first[1] >> 40, last[1] >> 40)
    return head + b''.join(REC.pack(k, v) for k, v in records)


def check(data, idx):
    magic, version, tag, length, count, fk, lk, fa, la, fs, ls = HEAD.unpack_from(idx, 0)
    ok = magic == b'BDKI' and version == 1 and tag == TAG and length <= len(data) and 0 < count < (16 << 20)
    ok = ok and len(idx) == HEAD.size + count * REC.size

    def head_at(at):
        if at < 0 or at + 16 > length:
            return None
        key, size = struct.unpack_from('<QI', data, at)
        return (key, size) if data[at + 12:at + 16] == b'DKSH' and 64 <= size <= (16 << 20) else None

    ok = ok and head_at(fa - 12) == (fk, fs) and head_at(la - 12) == (lk, ls) and la + ls == length
    return ok, count, length


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache')
    ap.add_argument('--out', default=None)
    ap.add_argument('--check', action='store_true', help='read the index back and check it against the cache')
    a = ap.parse_args()
    out = a.out or a.cache + '.idx'
    data = open(a.cache, 'rb').read()
    if a.check:
        ok, count, length = check(data, open(out, 'rb').read())
        print(f'deko3d_cache_index: {out}: {"matches" if ok else "does NOT match"} the cache ({count} records, '
              f'{length} bytes of records)')
        sys.exit(0 if ok else 1)
    index, end = walk(data)
    if end != len(data):
        print(f'deko3d_cache_index: {len(data) - end} bytes after the last whole record (the console cuts them)')
    blob = make_index(index, end)
    open(out, 'wb').write(blob)
    ok, count, length = check(data, blob)
    print(f'deko3d_cache_index: {count} records, {end} bytes -> {out} ({len(blob)} bytes){"" if ok else " CHECK FAILED"}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
