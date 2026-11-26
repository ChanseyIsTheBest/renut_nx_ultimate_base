#!/usr/bin/env python3
"""renut-ultimate (dmc_devil-nx's 4858b474, DMC_NACP_UTF8): sdk/cmake/patch_nacp_utf8.py on NACPs laid out as
nacptool --create writes them (16 language entries with the name and author, the version at 0x3060).
  python tools/tests/patch_nacp_utf8_test.py
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'sdk', 'cmake'))
import patch_nacp_utf8 as p  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def nacptool(title, author, version, filled=16):
    """Bytes as nacptool makes them (the strings as it receives them, here already bytes)."""
    buf = bytearray(p.NACP_SIZE)
    for i in range(filled):
        off = i * p.ENTRY_SIZE
        buf[off:off + len(title)] = title
        buf[off + p.NAME_SIZE:off + p.NAME_SIZE + len(author)] = author
    buf[p.VERSION_AT:p.VERSION_AT + len(version)] = version
    buf[0x3040] = 0x5A  # some other field: must not change
    return bytes(buf)


# ASCII: nothing to change, the file is left as it is.
base = nacptool(b'Beautiful Katamari', b'ChanseyIsTheBest', b'1.0.2')
check(p.patch(base, 'Beautiful Katamari', 'ChanseyIsTheBest', '1.0.2') is None, 'ASCII strings changed the NACP')

# UTF-8 author: every filled entry gets the exact bytes, the rest of the file stays.
ansi = nacptool(b'DmC - Devil May Cry', 'Bánh Mì'.encode('cp1252'), b'1.0.0')
out = p.patch(ansi, 'DmC - Devil May Cry', 'Bánh Mì', '1.0.0')
check(out is not None and len(out) == p.NACP_SIZE, 'no patched NACP')
want_author = 'Bánh Mì'.encode('utf-8')
for i in range(16):
    off = i * p.ENTRY_SIZE
    check(out[off:off + p.NAME_SIZE].rstrip(b'\0') == b'DmC - Devil May Cry', 'entry %d name' % i)
    check(out[off + p.NAME_SIZE:off + p.ENTRY_SIZE].rstrip(b'\0') == want_author, 'entry %d author' % i)
check(out[p.VERSION_AT:p.VERSION_AT + p.VERSION_SIZE].rstrip(b'\0') == b'1.0.0', 'version')
check(out[0x3040] == 0x5A, 'another field changed')
check(out[16 * p.ENTRY_SIZE:p.VERSION_AT] == ansi[16 * p.ENTRY_SIZE:p.VERSION_AT], 'bytes after the entries changed')

# Only the entries nacptool filled are written.
some = nacptool(b'x', b'y', b'1', filled=3)
out = p.patch(some, 'Ōkami', 'y', '1')
check(out[3 * p.ENTRY_SIZE:16 * p.ENTRY_SIZE] == bytes(13 * p.ENTRY_SIZE), 'an empty entry was written')

# A long name is cut at a character boundary and keeps its NUL.
long_name = 'é' * 600
out = p.patch(nacptool(b'a', b'b', b'1'), long_name, 'b', '1')
field = out[0:p.NAME_SIZE]
check(field[-1] == 0, 'the name has no NUL')
check(field.rstrip(b'\0').decode('utf-8') == 'é' * ((p.NAME_SIZE - 1) // 2), 'the name is not cut on a boundary')

# Files that are not as expected are left alone.
with tempfile.TemporaryDirectory() as d:
    nacp, meta = os.path.join(d, 'a.nacp'), os.path.join(d, 'meta.txt')
    open(nacp, 'wb').write(b'\1' * 100)
    open(meta, 'w', encoding='utf-8').write('Ōkami\nb\n1\n')
    check(p.main(['x', nacp, meta]) == 2 and open(nacp, 'rb').read() == b'\1' * 100, 'a wrong size was written')
    open(meta, 'w', encoding='utf-8').write('only a title\n')
    open(nacp, 'wb').write(base)
    check(p.main(['x', nacp, meta]) == 2 and open(nacp, 'rb').read() == base, 'a short meta file was used')
    open(meta, 'w', encoding='utf-8').write('Beautiful Katamari\nChanseyIsTheBest\n1.0.2\n')
    check(p.main(['x', nacp, meta]) == 0 and open(nacp, 'rb').read() == base, 'ASCII meta rewrote the file')
    open(meta, 'w', encoding='utf-8').write('DmC - Devil May Cry\nBánh Mì\n1.0.0\n')
    open(nacp, 'wb').write(ansi)
    check(p.main(['x', nacp, meta]) == 0 and want_author in open(nacp, 'rb').read(), 'the UTF-8 author is missing')
    check(p.main(['x', nacp]) == 2, 'wrong argument count accepted')

print('patch_nacp_utf8_test:', 'FAILED (%d)' % failures if failures else 'ok')
sys.exit(1 if failures else 0)
