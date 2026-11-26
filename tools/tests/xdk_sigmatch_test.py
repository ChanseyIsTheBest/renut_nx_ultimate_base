#!/usr/bin/env python3
"""renut-ultimate (2026-11-27, port 371): tools/xdk_sigmatch.py against a made-up generated tree and image.
- The `address` mask (the tables without a "mask" field, Blur's): two getters that differ only in the field they read
  share a signature, as do li 4096 and li 2048. The `struct` mask tells both pairs apart, and still matches two
  functions that build different addresses (lis + lwz).
- `make` without --mask writes no "mask" field; `find` reads such a table with the address mask.
- `hooks`: bound hooks only, named by their comment, through --names-from ("Katamari sub_X", "N&B sub_X", the handler)
  or --name; an unnamed bound hook writes nothing; "unique" and "xdk" recorded.
- `find` with an XDK table: refused without --xdk, places on its own XDK, only leads on another.
Exit 0: every check passed.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
import xdk_sigmatch as xs  # noqa: E402

BASE = 0x82000000


def lwz(rd, d, ra):
    return (32 << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF)


def lis(rd, imm):
    return (15 << 26) | (rd << 21) | (imm & 0xFFFF)


def li(rd, imm):
    return (14 << 26) | (rd << 21) | (imm & 0xFFFF)


BLR = 0x4E800020
FUNCS = {  # address: (words, comments)
    0x82000000: ([lwz(3, 100, 3), BLR], ['lwz r3,100(r3)', 'blr']),
    0x82000010: ([lwz(3, 104, 3), BLR], ['lwz r3,104(r3)', 'blr']),
    0x82000020: ([lis(11, 0x8200), lwz(3, 0x1234, 11), BLR], ['lis r11,-32256', 'lwz r3,4660(r11)', 'blr']),
    0x82000030: ([lis(11, 0x8201), lwz(3, 0x5678, 11), BLR], ['lis r11,-32255', 'lwz r3,22136(r11)', 'blr']),
    0x82000040: ([li(5, 4096), BLR], ['li r5,4096', 'blr']),
    0x82000050: ([li(5, 2048), BLR], ['li r5,2048', 'blr']),
}


def write_tree(d):
    gen = os.path.join(d, 'gen')
    os.makedirs(gen)
    with open(os.path.join(gen, 'x_recomp.0.cpp'), 'w', newline='\n') as f:
        for addr, (_, text) in sorted(FUNCS.items()):
            f.write('DEFINE_REX_FUNC(sub_%08X) {\n' % addr + ''.join('\t// %s\n\tstub();\n' % t for t in text) + '}\n\n')
    image = bytearray(0x100)
    for addr, (words, _) in FUNCS.items():
        struct.pack_into('>%dI' % len(words), image, addr - BASE, *words)
    path = os.path.join(d, 'image.bin')
    open(path, 'wb').write(bytes(image))
    return gen, path


def write_hooks(d, name, lines):
    h = os.path.join(d, name)
    os.makedirs(h)
    open(os.path.join(h, 'state.cpp'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    return h


def tool(*args):
    r = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'xdk_sigmatch.py')] + list(args),
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    fails = 0

    def check(what, ok):
        nonlocal fails
        if not ok:
            fails += 1
            print('FAIL', what)

    with tempfile.TemporaryDirectory() as d:
        gen, img = write_tree(d)
        image = open(img, 'rb').read()

        def sig(addr, mask):
            return xs.signature(image, addr, len(FUNCS[addr][0]), mask)

        check('address: field getters alike', sig(0x82000000, 'address') == sig(0x82000010, 'address'))
        check('struct: field getters apart', sig(0x82000000, 'struct') != sig(0x82000010, 'struct'))
        check('struct: built addresses alike', sig(0x82000020, 'struct') == sig(0x82000030, 'struct'))
        check('address: li constants alike', sig(0x82000040, 'address') == sig(0x82000050, 'address'))
        check('struct: li constants apart', sig(0x82000040, 'struct') != sig(0x82000050, 'struct'))
        check('out of the image: no signature', xs.signature(image, 0x83000000, 2, 'struct') is None)

        old = os.path.join(d, 'old.json')
        code, _ = tool('make', gen, img, 'getA=82000000', '--out', old)
        t = json.load(open(old))
        check('make: no mask field by default', code == 0 and 'mask' not in t['getA'])
        code, out = tool('find', gen, img, old)
        check('find: an old table with the address mask', code == 0 and '82000000, 82000010' in out)

        k = write_hooks(d, 'katamari', [
            'BANJO_RENUT_HOOK(sub_82000000, GetA_hook);  // D3DDevice_GetA  // katamari-nx: N&B sub_82990000, ok',
            'BANJO_RENUT_HOOK(sub_82000010, GetB_hook);  // D3DDevice_GetB  // katamari-nx: signature 1.00',
            'BANJO_RENUT_UNHOOKED_HOOK(sub_82000040, X_hook);  // D3DDevice_X  // katamari-nx: not hooked',
            'BANJO_RENUT_UNHOOKED_HOOK(sub_BeginIndexedVertices, Y_hook);  // D3DDevice_Y',
            'BANJO_RENUT_HOOK_RAW(sub_82000020) {',
        ])
        kt = os.path.join(d, 'k.json')
        code, out = tool('hooks', gen, img, k, '--xdk', '2.0.5632', '--out', kt)
        check('hooks: an unnamed bound hook writes nothing', code != 0 and not os.path.exists(kt) and '82000020' in out)
        code, out = tool('hooks', gen, img, k, '--xdk', '2.0.5632', '--out', kt, '--name', '82000020=D3DDevice_Raw')
        t = json.load(open(kt)) if code == 0 else {}
        check('hooks: bound ones named', sorted(t) == ['D3DDevice_GetA', 'D3DDevice_GetB', 'D3DDevice_Raw'])
        check('hooks: unbound left out', 'D3DDevice_X' not in t)
        check('hooks: unique, xdk, struct', t and t['D3DDevice_GetA']['unique'] is True and
              t['D3DDevice_GetA']['xdk'] == '2.0.5632' and t['D3DDevice_GetA']['mask'] == 'struct')
        check('hooks: a shared signature is not unique', t and t['D3DDevice_Raw']['unique'] is False)
        code, _ = tool('hooks', gen, img, k, '--out', kt + '2', '--name', '82000020=D3DDevice_Raw')
        check('hooks: --xdk required', code != 0)

        p = write_hooks(d, 'port', [
            'BANJO_RENUT_HOOK(sub_82000010, GetB_hook);  // port-nx: Katamari sub_82000010; render state getter',
            'BANJO_RENUT_HOOK(sub_82000000, GetA_hook);  // port-nx: N&B sub_82990000; placed by hand',
            'BANJO_RENUT_HOOK(sub_82000050, X_hook);  // port-nx: no reference',
        ])
        pt = os.path.join(d, 'p.json')
        code, out = tool('hooks', gen, img, p, '--xdk', '2.0.6534', '--out', pt, '--names-from', k)
        t = json.load(open(pt)) if code == 0 else {}
        check('names-from: by Katamari address, N&B address and handler',
              sorted(t) == ['D3DDevice_GetA', 'D3DDevice_GetB', 'D3DDevice_X'])

        code, out = tool('find', gen, img, kt)
        check('find: an XDK table needs --xdk', code != 0 and '--xdk' in out)
        code, out = tool('find', gen, img, kt, '--xdk', '2.0.5632')
        check('find: placed on its own XDK', code == 0 and 'D3DDevice_GetA       82000000' in out and 'lead' not in out)
        check('find: a non-unique entry is read by hand', 'D3DDevice_Raw        82000020, 82000030  (read by hand)' in out)
        code, out = tool('find', gen, img, kt, '--xdk', '2.0.6534')
        check('find: only leads on another XDK', code == 0 and out.count('(a lead: the table is XDK 2.0.5632') == 3)
    print('xdk_sigmatch_test:', 'ALL OK' if not fails else f'{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
