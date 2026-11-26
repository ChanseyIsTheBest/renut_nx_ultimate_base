#!/usr/bin/env python3
"""pokersmash-nx (2026-11-25): what a Windows minidump of the PC build (%LOCALAPPDATA%\\CrashDumps\\renut.exe.<pid>.dmp)
says without a debugger: the exception (code, address, its module offset, parameters), and the crashing thread's stack
words that point into the executable, named by llvm-symbolizer with its PDB (the PC build's /DEBUG:FULL). The generated
functions call each other on the host stack, so the guest chain shows up as sub_X frames (the guest stack has no return
addresses with elide_lr_stores). A scan, not an unwind: a stale word can show up too, so read the chain from the top.
Poker Smash's run 2 (a breakpoint 14 s in, no log line) was placed with it: rex::debug::Break <- the kernel's
DbgBreakPoint <- the game's main-thread processor check (kernel_main_thread_cpu0).

  python -I tools/minidump_stack.py DUMP [--exe app/out/pc/renut.exe] [--words 60] [--symbolizer PATH]

renut-ultimate (the hardening on the way in): the exception's parameters read at their offset (+40 of the stream; the
port read them 4 bytes late), a dump without an exception, thread or module stream says so, the executable is found by
--exe's file name and bounded by its module size, its preferred image base is read from its PE header (0x140000000 when
it cannot be read), the symbolizer is looked for as tools/census_sites.py does, and the reading is one function
(read_dump) that tools/tests/minidump_stack_test.py checks on a dump it builds.
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLIZERS = [os.path.join(os.path.dirname(ROOT), '_tools', 'llvm', 'bin', 'llvm-symbolizer.exe'),
               'C:/devkitPro/msys2/clang64/bin/llvm-symbolizer.exe', 'llvm-symbolizer']
STREAM_THREADS, STREAM_MODULES, STREAM_EXCEPTION = 3, 4, 6


class DumpError(Exception):
    pass


def read_dump(d, exe_name, max_words=60):
    """The minidump's exception and the crashing thread's stack words inside the module named exe_name:
    {'tid', 'code', 'address', 'module' (name, offset) or None, 'params', 'exe' (base, size), 'words' [absolute
    addresses, the exception's own first when it is in the module], 'stack' (bool)}."""
    if d[:4] != b'MDMP':
        raise DumpError('not a minidump')
    n, rva = struct.unpack_from('<II', d, 8)
    streams = {}
    for i in range(n):
        t, size, r = struct.unpack_from('<III', d, rva + 12 * i)
        streams[t] = (size, r)
    for need, what in ((STREAM_MODULES, 'module list'), (STREAM_EXCEPTION, 'exception'), (STREAM_THREADS, 'thread list')):
        if need not in streams:
            raise DumpError(f'no {what} stream (a dump written without it)')
    # MINIDUMP_MODULE (108 bytes): BaseOfImage, SizeOfImage, CheckSum, TimeDateStamp, ModuleNameRva ...
    mods = []
    _, r = streams[STREAM_MODULES]
    for i in range(struct.unpack_from('<I', d, r)[0]):
        o = r + 4 + 108 * i
        base, msize, _, _, name_rva = struct.unpack_from('<QIIII', d, o)
        nlen = struct.unpack_from('<I', d, name_rva)[0]
        mods.append((base, msize, d[name_rva + 4:name_rva + 4 + nlen].decode('utf-16-le', 'replace')))

    def where(addr):
        for base, msize, name in mods:
            if base <= addr < base + msize:
                return base, os.path.basename(name)
        return None, None
    exe = next(((b, s) for b, s, nm in mods if os.path.basename(nm.replace('\\', '/')).lower() == exe_name.lower()),
               None)
    # MINIDUMP_EXCEPTION_STREAM: ThreadId, alignment, MINIDUMP_EXCEPTION (code, flags, record, address,
    # NumberParameters, alignment, ExceptionInformation[15]: 152 bytes), then the thread context's location.
    _, r = streams[STREAM_EXCEPTION]
    tid = struct.unpack_from('<I', d, r)[0]
    code, _flags, _rec, addr, nparams = struct.unpack_from('<IIQQI', d, r + 8)
    params = list(struct.unpack_from('<15Q', d, r + 40))[:min(nparams, 15)]
    ctx_size, ctx_rva = struct.unpack_from('<II', d, r + 8 + 152)
    rsp = struct.unpack_from('<Q', d, ctx_rva + 0x98)[0] if ctx_size >= 0xA0 else 0  # AMD64 CONTEXT: Rsp at 0x98
    b, m = where(addr)
    out = {'tid': tid, 'code': code, 'address': addr, 'module': (m, addr - b) if b is not None else None,
           'params': params, 'exe': exe, 'words': [], 'stack': False}
    # MINIDUMP_THREAD (48 bytes): ThreadId ... Teb, then Stack (StartOfMemoryRange, DataSize, Rva) at +24
    _, r = streams[STREAM_THREADS]
    stack = None
    for i in range(struct.unpack_from('<I', d, r)[0]):
        o = r + 4 + 48 * i
        if struct.unpack_from('<I', d, o)[0] == tid:
            start, ssize, srva = struct.unpack_from('<QII', d, o + 24)
            stack = (start, d[srva:srva + ssize])
    if exe is None:
        return out
    exe_base, exe_size = exe
    words = [addr] if where(addr)[0] == exe_base else []
    if stack and rsp:
        out['stack'] = True
        start, mem = stack
        off = max(0, rsp - start)
        off -= off % 8
        for i in range(off, len(mem) - 7, 8):
            w = struct.unpack_from('<Q', mem, i)[0]
            if exe_base + 0x1000 < w < exe_base + exe_size:
                words.append(w)
                if len(words) > max_words:
                    break
    out['words'] = words
    return out


def image_base(exe):
    """The PE header's preferred ImageBase (llvm-symbolizer takes addresses at it), or the x64 default."""
    try:
        with open(exe, 'rb') as f:
            head = f.read(4096)
        pe = struct.unpack_from('<I', head, 0x3C)[0]
        if head[pe:pe + 4] != b'PE\0\0':
            return 0x140000000
        opt = pe + 24
        magic = struct.unpack_from('<H', head, opt)[0]
        if magic == 0x20B:  # PE32+
            return struct.unpack_from('<Q', head, opt + 24)[0]
        if magic == 0x10B:  # PE32
            return struct.unpack_from('<I', head, opt + 28)[0]
    except (OSError, struct.error):
        pass
    return 0x140000000


def symbolizer(given):
    for s in ([given] if given else []) + SYMBOLIZERS:
        if os.path.isfile(s) or (os.sep not in s and '/' not in s):
            return s
    return 'llvm-symbolizer'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'app', 'out', 'pc', 'renut.exe'))
    ap.add_argument('--words', type=int, default=60)
    ap.add_argument('--symbolizer', default='')
    a = ap.parse_args()
    exe_name = os.path.basename(a.exe)
    try:
        r = read_dump(open(a.dump, 'rb').read(), exe_name, a.words)
    except (DumpError, struct.error) as e:
        sys.exit(f'{a.dump}: {e}')
    where = f' ({r["module"][0]}+{r["module"][1]:#x})' if r['module'] else ''
    print(f'thread {r["tid"]}: exception {r["code"]:08X} at {r["address"]:#x}{where}')
    print('  parameters', ' '.join(f'{p:#x}' for p in r['params']))
    if r['exe'] is None:
        sys.exit(f'no module named {exe_name} in the dump (--exe names the executable)')
    if not r['stack']:
        print(f'  no stack of thread {r["tid"]} in the dump')
    if not r['words']:
        print('  no stack word points into', exe_name)
        return
    exe_base = r['exe'][0]
    pref = image_base(a.exe)
    q = '\n'.join(hex(pref + (x - exe_base)) for x in r['words']) + '\n'
    try:
        out = subprocess.run([symbolizer(a.symbolizer), f'--obj={a.exe}'], input=q, capture_output=True,
                             text=True).stdout
    except OSError as e:
        print(f'  (llvm-symbolizer did not run: {e}); the module offsets:')
        print('  ' + ' '.join(hex(x - exe_base) for x in r['words']))
        return
    for f in [blk.split('\n') for blk in out.strip().split('\n\n') if blk.strip()]:
        print('  ' + ' | '.join(x.strip() for x in f[:2]))


if __name__ == '__main__':
    main()
