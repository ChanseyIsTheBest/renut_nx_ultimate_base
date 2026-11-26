#!/usr/bin/env python3
"""The XDK's statically linked library functions found by signature in any ReXGlue port (blur-nx, 2026-11-17; #251 of
docs/cpu-codegen-universal-30-1116.md). Every Xbox 360 game links the same XDK libraries (the C runtime, XMCORE, XAPILIB,
D3D9, D3DX9, XAUDIO2, ...) per XDK version: the same machine code at other addresses. A native written for one game's
copy of such a function (Blur's memset, memcpy and the large memcpy, app/src/cpu/hot/blur_hand_natives_crt.h and
blur_hand_natives_audio2.h) serves every game of that XDK version once its address there is known. This finds it.

A signature is the function's instruction words with what relocation changes masked: branch targets (b, bl, bc),
the immediates of addi / addis / ori / oris (address building), and the displacements of loads and stores not
relative to r1 (an address's low half). Registers, opcodes and stack offsets stay. Two functions match when their
instruction counts and signatures are equal. Function boundaries and sizes come from the port's generated code
(DEFINE_REX_FUNC(sub_X) and one `// insn` comment per guest instruction), the words from its image dump
(out/renut_image.bin, REXGLUE_DUMP_IMAGE; base 0x82000000).

  python tools/xdk_sigmatch.py make GEN IMAGE NAME=ADDR [NAME=ADDR ...] --out TABLE.json [--mask address|struct]
  python tools/xdk_sigmatch.py hooks GEN IMAGE HOOKDIR --xdk 2.0.NNNN --out TABLE.json [--names-from HOOKDIR]
         [--name ADDR=NAME ...] [--game LABEL]
  python tools/xdk_sigmatch.py find GEN IMAGE TABLE.json [--xdk 2.0.NNNN] [--summary]
e.g. python tools/xdk_sigmatch.py make app/generated out/renut_image.bin memset=82953880 memcpy=82954218 ^
       memcpy_large=82953D90 --out tools/xdk_2.0.11164_sigs.json

renut-ultimate (2026-11-27, port 371): the `struct` mask and the `hooks` table of a port's Renut hooks.
- The `address` mask above (every table without a "mask" field, Blur's among them: unchanged) also masks a device's
  field offsets (lwz r3,12436(r31) is not r1-relative), so the Direct3D getters and setters that differ only in the
  field they touch share one signature: 101 of Katamari's 321 bound hooks were ambiguous in Katamari itself. The
  `struct` mask masks a displacement or an addi / ori immediate only when its base register is set by an addis (lis)
  in the function: an address being built. Field offsets and li constants stay. Katamari: 317 of 321 unique (Poker
  Smash 319 of 322). The price: it matches only within one XDK's device layout, which is what a per-XDK table is.
- `hooks` reads a port's app/src/renut_engine/gpu/d3d/hooks: every bound hook (BANJO_RENUT_HOOK, _RAW, REX_HOOK_RAW;
  not the UNHOOKED ones), named by the leading D3D name of its comment (Katamari's form), else through --names-from's
  lines (a port's "Katamari sub_X" or "N&B sub_X", else its handler when that is unique there), else --name ADDR=NAME.
  An unnamed bound hook stops the table (nothing written). Each entry is a `struct` signature with "unique" (the
  signature belongs to this one function in its own game), "xdk" and "game". A bound hook is one the port's own builds
  run. tools/xdk_2.0.5632_renut_sigs.json is Katamari's (this base, 323 hooks), tools/xdk_2.0.6534_renut_sigs.json
  pokersmash-nx 1.0.0's (93873e35; 326, its state block capture / apply and shader GetFunction among them).
- `find` with such a table needs the game's --xdk. On the table's own XDK an address found once is the hook's place
  (each table on its own game: every unique entry back at its own address, none wrong); more than once, or an entry
  with "unique": false, is read by hand; not found is the fuzzy tools' (renut_sigmatch.py, renut_best_match.py,
  d3d_align.py). On another XDK every hit is a lead, never a place: Katamari's table on Poker Smash placed 175 right and
  9 wrong (Poker Smash's on Katamari 174 and 9), all of the wrong ones render-state getters, `lwz r3,N(r3); blr` with
  the device's state fields moved between the XDKs. Check a lead's field against the game's own state table.
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$')
INSN = re.compile(r'^\t// [a-z]')
MASKS = ('address', 'struct')


def sizes(gen):
    """{address: instruction count} of every generated function (its `// insn` comment lines)."""
    out = {}
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        cur = None
        n = 0
        for line in open(path, encoding='utf-8', errors='replace'):
            m = DEF.match(line)
            if m:
                cur, n = int(m.group(1), 16), 0
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                out[cur] = n
                cur = None
            elif INSN.match(line):
                n += 1
    return out


def masked(w):
    op = w >> 26
    if op == 18:  # b, bl: the target
        return w & ~0x03FFFFFC
    if op == 16:  # bc: the target
        return w & ~0x0000FFFC
    if op in (14, 15, 24, 25):  # addi, addis, ori, oris: an address's halves
        return w & ~0xFFFF
    ra = (w >> 16) & 31
    if 32 <= op <= 55 or op in (58, 62):  # D / DS-form loads and stores
        return w if ra == 1 else w & ~0xFFFF
    return w


def addis_regs(words):
    """The registers an addis (lis) sets anywhere in the function: the ones that hold an address being built."""
    return {(w >> 21) & 31 for w in words if (w >> 26) == 15}


def masked_struct(w, regs):
    op = w >> 26
    if op == 18:
        return w & ~0x03FFFFFC
    if op == 16:
        return w & ~0x0000FFFC
    if op == 15:  # addis / lis: an address's high half
        return w & ~0xFFFF
    ra = (w >> 16) & 31
    if op == 14:  # addi: an address's low half when its base is a lis register; li (ra 0) and offsets stay
        return w & ~0xFFFF if ra in regs else w
    if op in (24, 25):  # ori, oris: the source is rS (bits 21-25)
        return w & ~0xFFFF if ((w >> 21) & 31) in regs else w
    if 32 <= op <= 55 or op in (58, 62):
        return w & ~0xFFFF if ra in regs else w
    return w


def signature(image, addr, count, mask='address'):
    off = addr - IMAGE_BASE
    if off < 0 or count <= 0 or off + 4 * count > len(image):
        return None
    words = struct.unpack_from('>%dI' % count, image, off)
    h = hashlib.sha1()
    if mask == 'struct':
        regs = addis_regs(words)
        for w in words:
            h.update(struct.pack('>I', masked_struct(w, regs)))
    else:
        for w in words:
            h.update(struct.pack('>I', masked(w)))
    return h.hexdigest()


def finder(image, sz):
    """find(signature, count, mask) -> every address of that size with that signature (cached)."""
    by_size = collections.defaultdict(list)
    for addr, n in sz.items():
        by_size[n].append(addr)
    cache = {}

    def find(sig, count, mask):
        hits = []
        for addr in by_size.get(count, []):
            key = (addr, mask)
            if key not in cache:
                cache[key] = signature(image, addr, count, mask)
            if cache[key] == sig:
                hits.append(addr)
        return hits
    return find


def cmd_make(a):
    image = open(a.image, 'rb').read()
    sz = sizes(a.gen)
    table = {}
    for item in a.items:
        name, addr = item.split('=')
        addr = int(addr, 16)
        if addr not in sz:
            sys.exit('%s: no generated function at %08X' % (name, addr))
        table[name] = {'address': '%08X' % addr, 'instructions': sz[addr],
                       'signature': signature(image, addr, sz[addr], a.mask)}
        if a.mask != 'address':
            table[name]['mask'] = a.mask
    json.dump(table, open(a.out, 'w'), indent=1)
    print('%d signatures written to %s' % (len(table), a.out))


# --- a port's Renut hooks -----------------------------------------------------------------------------------------

HOOK_LINE = re.compile(r'^\s*(BANJO_RENUT_(?:UNHOOKED_)?HOOK(?:_RAW|_SHARED)?|REX_HOOK_RAW|REX_HOOK)\(\s*'
                       r'(sub_[0-9A-F]{8}|[A-Za-z_][A-Za-z0-9_]*)\s*(?:,\s*(.*?))?\)\s*;?\s*\{?\s*(?://\s*(.*))?$')
D3D_NAME = re.compile(r'^((?:Direct3D|D3D|XG|Xe|Vd|Xam)[A-Za-z0-9_]*)\b')
REF = re.compile(r'\b(Katamari|N&B) sub_([0-9A-F]{8})\b')


def read_hooks(d):
    """Every hook line of a hooks folder: dict(addr or None, bound, handler, comment, file, line)."""
    out = []
    for path in sorted(glob.glob(os.path.join(d, '*.cpp'))):
        for i, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
            m = HOOK_LINE.match(line)
            if not m:
                continue
            macro, target, handler, comment = m.groups()
            addr = int(target[4:], 16) if re.fullmatch(r'sub_[0-9A-F]{8}', target) else None
            out.append(dict(addr=addr, bound='UNHOOKED' not in macro and addr is not None, handler=(handler or '').strip(),
                            comment=(comment or '').strip(), file=os.path.basename(path), line=i))
    return out


def leading_name(comment):
    m = D3D_NAME.match(comment)
    return m.group(1) if m else None


def name_maps(hooks):
    """From a Katamari-style hooks folder: names by its own address, by the N&B address its comment gives, by handler."""
    by_addr, by_nb, by_handler = {}, {}, collections.defaultdict(set)
    for h in hooks:
        name = leading_name(h['comment'])
        if not name:
            continue
        if h['addr'] is not None:
            by_addr[h['addr']] = name
        for who, addr in REF.findall(h['comment']):
            if who == 'N&B':
                by_nb[int(addr, 16)] = name
        if h['handler']:
            by_handler[h['handler']].add(name)
    return by_addr, by_nb, {k: next(iter(v)) for k, v in by_handler.items() if len(v) == 1}


def cmd_hooks(a):
    image = open(a.image, 'rb').read()
    sz = sizes(a.gen)
    hooks = [h for h in read_hooks(a.hookdir) if h['bound']]
    if not hooks:
        sys.exit('%s: no bound hook' % a.hookdir)
    by_addr, by_nb, by_handler = name_maps(read_hooks(a.names_from)) if a.names_from else ({}, {}, {})
    forced = {}
    for item in a.name or []:
        addr, name = item.split('=', 1)
        forced[int(addr, 16)] = name
    table, unnamed, missing = {}, [], []
    find = finder(image, sz)
    for h in sorted(hooks, key=lambda h: h['addr']):
        name = forced.get(h['addr']) or leading_name(h['comment'])
        if not name:
            for who, ref in REF.findall(h['comment']):
                ref = int(ref, 16)
                name = by_addr.get(ref) if who == 'Katamari' else by_nb.get(ref)
                if name:
                    break
        if not name and h['handler']:
            name = by_handler.get(h['handler'])
        if not name:
            unnamed.append(h)
            continue
        if h['addr'] not in sz:
            missing.append((name, h))
            continue
        n = sz[h['addr']]
        sig = signature(image, h['addr'], n, 'struct')
        if sig is None:
            missing.append((name, h))
            continue
        key, k = name, 2
        while key in table:
            key, k = '%s#%d' % (name, k), k + 1
        table[key] = {'address': '%08X' % h['addr'], 'instructions': n, 'signature': sig, 'mask': 'struct',
                      'unique': find(sig, n, 'struct') == [h['addr']], 'xdk': a.xdk, 'game': a.game}
    for h in unnamed:
        print('unnamed: %08X %s:%d %s' % (h['addr'], h['file'], h['line'], h['comment'][:80]), file=sys.stderr)
    for name, h in missing:
        print('not in the image or the generated code: %s %08X' % (name, h['addr']), file=sys.stderr)
    if unnamed or missing:
        sys.exit('nothing written: name the hooks above (--name ADDR=NAME) or check GEN / IMAGE')
    json.dump(dict(sorted(table.items())), open(a.out, 'w'), indent=1)
    amb = sum(1 for e in table.values() if not e['unique'])
    print('%d hook signatures written to %s (%d not unique in their own game)' % (len(table), a.out, amb))


def cmd_find(a):
    image = open(a.image, 'rb').read()
    sz = sizes(a.gen)
    table = json.load(open(a.table))
    xdks = {e['xdk'] for e in table.values() if e.get('xdk')}
    if xdks and not a.xdk:
        sys.exit('%s is a table of XDK %s: give the game\'s XDK (--xdk 2.0.NNNN). Its hits are exact only on that XDK '
                 '(across 2.0.5632 and 2.0.6534 nine render-state getters a way matched the wrong state: the device\'s '
                 'fields moved)' % (a.table, ', '.join(sorted(xdks))))
    find = finder(image, sz)
    once = more = none = leads = 0
    for name, e in sorted(table.items()):
        mask = e.get('mask', 'address')
        if mask not in MASKS:
            sys.exit('%s: unknown mask %r in %s' % (name, mask, a.table))
        hits = find(e['signature'], e['instructions'], mask)
        other = bool(e.get('xdk')) and e['xdk'] != a.xdk
        if other and hits:
            leads += 1
        else:
            once += len(hits) == 1
            more += len(hits) > 1
        none += not hits
        if not a.summary:
            text = ', '.join('%08X' % h for h in hits) if hits else 'not found'
            if other and hits:
                text += '  (a lead: the table is XDK %s, the game %s)' % (e['xdk'], a.xdk)
            elif len(hits) > 1 or (hits and e.get('unique') is False):
                text += '  (read by hand)'
            print('%-20s %s' % (name, text))
    if a.summary or len(table) > 20:
        print('%d of %d found once, %d more than once, %d leads from another XDK, %d not found'
              % (once, len(table), more, leads, none))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make')
    m.add_argument('gen')
    m.add_argument('image')
    m.add_argument('items', nargs='+')
    m.add_argument('--out', required=True)
    m.add_argument('--mask', choices=MASKS, default='address')
    h = sub.add_parser('hooks')
    h.add_argument('gen')
    h.add_argument('image')
    h.add_argument('hookdir')
    h.add_argument('--out', required=True)
    h.add_argument('--names-from', help='a Katamari-style hooks folder that names a port\'s "Katamari sub_X" lines')
    h.add_argument('--name', action='append', help='ADDR=NAME for a bound hook its comment does not name')
    h.add_argument('--game', default='', help='the game and build the table comes from')
    h.add_argument('--xdk', required=True, help='the game\'s XDK version, 2.0.NNNN')
    f = sub.add_parser('find')
    f.add_argument('gen')
    f.add_argument('image')
    f.add_argument('table')
    f.add_argument('--summary', action='store_true', help='only the counts')
    f.add_argument('--xdk', help='the game\'s XDK version (needed for a table that names its XDK)')
    a = ap.parse_args()
    {'make': cmd_make, 'hooks': cmd_hooks, 'find': cmd_find}[a.cmd](a)


if __name__ == '__main__':
    main()
