#!/usr/bin/env python3
"""renut-ultimate (pokersmash-nx's tools/minidump_stack.py): read_dump on a minidump built here with the three streams
it reads (thread list 3, module list 4, exception 6), laid out as MINIDUMP_* in Windows' minidumpapiset.h: the
exception's code, address and parameters (ExceptionInformation at +40 of the stream), the module the address is in,
the crashing thread's stack words inside the executable from its Rsp up, and a dump without the exception stream.
No symbolizer is run.
  python tools/tests/minidump_stack_test.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import minidump_stack as m  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


EXE_BASE, EXE_SIZE = 0x7FF600000000, 0x02000000
DLL_BASE, DLL_SIZE = 0x7FFA00000000, 0x00100000
STACK_START = 0x000000D0_00100000


def build(with_exception=True):
    body = bytearray()
    head_size = 32
    streams = []

    def put(data):
        nonlocal body
        at = head_size + 3 * 12 + len(body)
        body += data
        while len(body) % 8:
            body += b'\0'
        return at
    # module names (MINIDUMP_STRING: length in bytes, UTF-16)
    names = {}
    for nm in ('C:\\game\\renut.exe', 'C:\\Windows\\System32\\ntdll.dll'):
        u = nm.encode('utf-16-le')
        names[nm] = put(struct.pack('<I', len(u)) + u + b'\0\0')
    mods = struct.pack('<I', 2)
    for base, size, nm in ((EXE_BASE, EXE_SIZE, 'C:\\game\\renut.exe'),
                           (DLL_BASE, DLL_SIZE, 'C:\\Windows\\System32\\ntdll.dll')):
        mods += struct.pack('<QIIII', base, size, 0, 0, names[nm]) + b'\0' * (108 - 24)
    streams.append((4, len(mods), put(mods)))
    # the crashing thread's stack: two exe words, a dll word, an exe word below Rsp (must be skipped)
    rsp = STACK_START + 0x40
    words = [0] * 32
    words[2] = EXE_BASE + 0x500  # below Rsp: not read
    words[8] = EXE_BASE + 0x123450  # at Rsp
    words[9] = DLL_BASE + 0x10
    words[11] = EXE_BASE + 0x0800  # under +0x1000: the header, not code
    words[12] = EXE_BASE + 0x200000
    stack = struct.pack('<32Q', *words)
    stack_rva = put(stack)
    # the AMD64 CONTEXT (only Rsp at 0x98 is read)
    ctx = bytearray(0x4D0)
    struct.pack_into('<Q', ctx, 0x98, rsp)
    ctx_rva = put(bytes(ctx))
    threads = struct.pack('<I', 1) + struct.pack('<IIIIQ', 77, 0, 0, 0, 0) + \
        struct.pack('<QII', STACK_START, len(stack), stack_rva) + struct.pack('<II', len(ctx), ctx_rva)
    streams.append((3, len(threads), put(threads)))
    if with_exception:
        ex = struct.pack('<II', 77, 0) + struct.pack('<IIQQII', 0xC0000005, 0, 0, EXE_BASE + 0x4242, 2, 0) + \
            struct.pack('<15Q', 1, 0x48A10070, *([0xDEAD] * 13)) + struct.pack('<II', len(ctx), ctx_rva)
        streams.append((6, len(ex), put(ex)))
    while len(streams) < 3:
        streams.append((0, 0, 0))  # UnusedStream
    head = struct.pack('<4sIIIIIQ', b'MDMP', 0xA793, 3, head_size, 0, 0, 0)
    directory = b''.join(struct.pack('<III', t, s, r) for t, s, r in streams)
    return head + directory + bytes(body)


r = m.read_dump(build(), 'renut.exe')
check(r['tid'] == 77, f'thread id {r["tid"]}')
check(r['code'] == 0xC0000005, f'code {r["code"]:#x}')
check(r['address'] == EXE_BASE + 0x4242, 'address')
check(r['module'] == ('renut.exe', 0x4242), f'module {r["module"]}')
check(r['params'] == [1, 0x48A10070], f'parameters {r["params"]} (ExceptionInformation at +40)')
check(r['exe'] == (EXE_BASE, EXE_SIZE), 'the executable by its file name and size')
check(r['stack'], 'the crashing thread\'s stack found')
check(r['words'] == [EXE_BASE + 0x4242, EXE_BASE + 0x123450, EXE_BASE + 0x200000],
      f'stack words {[hex(w) for w in r["words"]]} (the exception first, then from Rsp up, inside the exe only)')
r2 = m.read_dump(build(), 'other.exe')
check(r2['exe'] is None and r2['words'] == [], 'an executable not in the dump gives no words')
try:
    m.read_dump(build(with_exception=False), 'renut.exe')
    check(False, 'a dump without an exception stream was read')
except m.DumpError:
    pass
try:
    m.read_dump(b'NOPE' + b'\0' * 64, 'renut.exe')
    check(False, 'a file that is not a minidump was read')
except m.DumpError:
    pass

print('minidump_stack_test:', 'ALL OK' if not failures else f'{failures} FAILED')
sys.exit(1 if failures else 0)
