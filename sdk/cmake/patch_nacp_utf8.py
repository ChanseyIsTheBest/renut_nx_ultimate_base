#!/usr/bin/env python3
"""dmc_devil-nx (4858b474; its notes: "patch_nacp_utf8.py for UTF-8 NACP on Windows"): the NRO's title, author and
version written into the NACP as UTF-8 after nacptool made it. On a Windows host nacptool.exe gets its command line in
the ANSI code page, so a name such as "Bánh Mì" reached the NACP as other bytes. sdk/cmake/rexglue_switch.cmake writes
the three strings to a UTF-8 file at configure time and runs this after nacptool (DMC_NACP_UTF8, ON).

renut-ultimate's hardening of the port's script: three ASCII strings (nacptool's bytes are then already exact) leave the
file untouched; a string is cut at a character boundary, so the field never ends in half a character; only the language
entries nacptool filled are written; a file or argument that is not as expected is left alone (exit 2).

The NACP (0x4000 bytes): 16 language entries of 0x300 bytes (name 0x200, author 0x100), the display version at 0x3060
(0x10 bytes). Each field keeps a NUL after its text.
  python patch_nacp_utf8.py <nacp> <meta_utf8.txt>     (meta: title, author and version, a line each)
"""
import sys

NACP_SIZE = 0x4000
ENTRIES = 16
ENTRY_SIZE = 0x300
NAME_SIZE = 0x200
AUTHOR_SIZE = 0x100
VERSION_AT = 0x3060
VERSION_SIZE = 0x10


def fit(text, room):
    """text's UTF-8 bytes, cut at a character boundary to at most room - 1 bytes (the field's NUL stays)."""
    out = bytearray()
    for ch in text:
        b = ch.encode('utf-8')
        if len(out) + len(b) > room - 1:
            break
        out += b
    return bytes(out)


def patch(buf, title, author, version):
    """The NACP bytes with the three strings in UTF-8, or None when nothing changes (all three ASCII)."""
    if len(buf) != NACP_SIZE:
        raise ValueError('unexpected NACP size %d' % len(buf))
    if title.isascii() and author.isascii() and version.isascii():
        return None
    out = bytearray(buf)
    name_b, author_b = fit(title, NAME_SIZE), fit(author, AUTHOR_SIZE)
    for i in range(ENTRIES):
        off = i * ENTRY_SIZE
        if any(out[off:off + ENTRY_SIZE]):  # the entries nacptool filled
            out[off:off + NAME_SIZE] = name_b.ljust(NAME_SIZE, b'\0')
            out[off + NAME_SIZE:off + ENTRY_SIZE] = author_b.ljust(AUTHOR_SIZE, b'\0')
    out[VERSION_AT:VERSION_AT + VERSION_SIZE] = fit(version, VERSION_SIZE).ljust(VERSION_SIZE, b'\0')
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write('usage: patch_nacp_utf8.py <nacp> <meta_utf8.txt>\n')
        return 2
    try:
        lines = open(argv[2], encoding='utf-8').read().splitlines()
    except (OSError, UnicodeDecodeError) as e:
        sys.stderr.write('patch_nacp_utf8: %s: %s\n' % (argv[2], e))
        return 2
    if len(lines) < 3:
        sys.stderr.write('patch_nacp_utf8: %s: a title, an author and a version expected\n' % argv[2])
        return 2
    try:
        with open(argv[1], 'rb') as f:
            buf = f.read()
        new = patch(buf, lines[0], lines[1], lines[2])
    except (OSError, ValueError) as e:
        sys.stderr.write('patch_nacp_utf8: %s: %s; left as nacptool wrote it\n' % (argv[1], e))
        return 2
    if new is None:
        return 0
    with open(argv[1], 'wb') as f:
        f.write(new)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
