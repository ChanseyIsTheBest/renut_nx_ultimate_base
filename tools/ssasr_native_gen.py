#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the second batch of natives; the user: "can I have 10 more hand natives before testing on
console again on the main game thread"): hand-written natives made from a function's machine copy by rewrites that are
exact by construction (the guide's method: docs/hand-and-machine-optimising-guide.md 3.2), for the per-draw functions
of the main thread whose samples sit on costs the generated code cannot see past:

  raw   a 16-byte vector loaded (lvx: byte-swapped into the register) and stored again unchanged (stvx: swapped back)
        is copied as its bytes: the store takes the loaded bytes, the register still gets the swapped value (the
        compiler drops it where nothing reads it). Exact: swap(swap(x)) = x, and the register is not written between.
  fwd   a stack word stored and loaded back (std / lfd for an integer's conversion, stw / lwz spills) with no label, no
        call and no other store in between: the load takes the stored value; the store stays (the bytes are the
        original's). Exact: nothing can change the word in between.
  cards the per-store card marks of stores through one base register that no statement of the function changes are
        marked once, at each return, for the span the stores cover (marked when the function ends, before any call
        or return; a call or a fence in between keeps the marks as they are: off for that function).

    python tools/ssasr_native_gen.py --report FUNC ...        # the rewrites each function's copy allows
    python tools/ssasr_native_gen.py FUNC[:KEY] ...            # writes app/generated/hot/ssasr_native_FUNC.h

The header holds banjo::hand::n_FUNC::Native<C, S> (C: the copy's callees, hand.txt calls=), the copy's function with
the rewrites, its key ssasr_hand_FUNC through banjo_hand_natives.h. Generated (the copy's game code: not committed);
tools/post_codegen.py runs this for the functions of app/src/cpu/hot/ssasr_natives.txt.
"""
import argparse
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOT = os.path.join(ROOT, 'app', 'generated', 'hot')
LIST = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'ssasr_natives.txt')

VLOAD = re.compile(r'^(\s*)simde_mm_store_si128\(\(simde__m128i\*\)(\w+)\.u8, REX_BSWAP128\(simde_mm_load_si128\('
                   r'\(simde__m128i\*\)REX_RAW_ADDR\(ea\)\)\)\);$')
VSTORE = re.compile(r'^(\s*)if \(s\.Note\(\(u32\)\(ea\), 16\)\) (REX_CARD\(ea\), )?simde_mm_store_si128\(\(simde__m128i\*\)'
                    r'REX_RAW_ADDR\(ea\), REX_BSWAP128\(simde_mm_load_si128\(\(simde__m128i\*\)(\w+)\.u8\)\)\);$')
EA = re.compile(r'^\s*ea = .*;$')
SSTORE = re.compile(r'^(\s*)BANJO_HOT_STORE_D_U(32|64)\((l_r1|ll_r1)\.u32, (-?\d+), (\w+)\.(u32|u64)\);$')
SLOAD = re.compile(r'^(\s*)(\w+)\.(u64) = REX_LOAD_D_U(32|64)\((l_r1|ll_r1)\.u32, (-?\d+)\);$')
LABEL = re.compile(r'^(loc_\w+):$')
# (ssasr-nx, loads batch M18: and a member's call that passes registers by reference, (ctx, base, s, l_r3, ...))
CALLISH = re.compile(r'\(ctx, base(, s)?(, [\w.]+)*\)|REX_CALL_INDIRECT|REX_GUEST_CAS|__atomic|REX_FENCE|ppc_trap')
STOREISH = re.compile(r'BANJO_HOT_STORE|REX_STORE|simde_mm_store_si128\(\(simde__m128i\*\)REX_RAW_ADDR|REX_STV|REX_DCBZ|'
                      r'REX_CHAIN_STORE|STACK_FWD|memset|memcpy')


def body_of(fn, report=False):
    path = os.path.join(HOT, f'n_{fn}.h')
    lines = open(path, encoding='utf-8').read().split('\n')
    head = re.compile(r'(\[\[[\w:, ]+\]\] )?static void f_sub_' + fn +
                      r'\(PPCContext& __restrict ctx, uint8_t\* base, \[\[maybe_unused\]\] S& s\) \{$')
    i = next(k for k, l in enumerate(lines) if head.match(l))
    j = next(k for k in range(i + 1, len(lines)) if lines[k] == '}')
    # Only a call of this function's own native goes (there is none in the body: the hook wrappers make it). A
    # callee's native stays: its line IS the call (if (!...::n_X::Native(...)) { f_X(...); ... }); dropping it, as
    # the first version did, dropped the calls of memcpy and of the derived-matrix cache, and the guard found it.
    body = [l for l in lines[i + 1:j] if f'::banjo::hand::n_{fn}::' not in l]
    # ssasr-nx (2026-11-27, loads batch M19; M18 refused such a body): a member call by reference (M17's form) is a
    # call like the others: the
    # native calls it as C::sub_X(ctx, base, s, l_r3, ...) through its Calls struct (localize_native.py's calls_struct
    # forwards the references), and CALLISH stops the rewrites at it (the member may write what it takes).
    return body, lines


def writes_vector(line, v):
    return re.search(r'simde_mm_store_(?:si128|ps)\((?:\(simde__m128i\*\))?' + re.escape(v) + r'\.(?:u8|u32|f32|u16), ', line) \
        is not None or re.search(r'\b' + re.escape(v) + r' = ', line) is not None or \
        re.search(r'\b' + re.escape(v) + r'\.(?:u8|u32|f32)\[', line) is not None and '=' in line.split(v)[1][:20]


SPLAT = re.compile(r'^(\s*)simde_mm_store_si128\(\(simde__m128i\*\)(\w+)\.u32, simde_mm_set1_epi32\(int\(((?:0x)?[0-9A-Fa-f]+)\)\)\);(.*)$')


def rewrite(body, opaque=False):
    """The rewrites; returns (new lines, counts, raw registers to declare)."""
    out = list(body)
    counts = {'raw': 0, 'fwd': 0, 'opaque': 0}
    if opaque:
        # Every splat constant (vspltisw, and tools/vec_const_fold.py's known conversions) through an empty asm, so the
        # compiler cannot fold arithmetic with it: the original statements hold these in the context, where it cannot
        # either. On the Switch the copies' folds gave a NaN the other sign (the ARM guard, 2026-11-26: e.g. a multiply by
        # a known -1.0 as a negation); these natives must give the original's bits.
        for k, l in enumerate(out):
            m = SPLAT.match(l)
            if m:
                ind, v, k_, rest = m.groups()
                out[k] = (f'{ind}{{ simde__m128i rex_k = simde_mm_set1_epi32(int({k_})); __asm__ volatile("" : "+m"(rex_k)); '
                          f'simde_mm_store_si128((simde__m128i*){v}.u32, rex_k); }}{rest}')
                counts['opaque'] += 1
    raws = set()
    # raw: a load into V (ea = A; VLOAD) and, later in the same straight run, a store of V (ea = B; VSTORE), V unchanged
    i = 0
    n = len(out)
    while i < n:
        m = VLOAD.match(out[i])
        if m and i > 0 and EA.match(out[i - 1]):
            v = m.group(2)
            j = i + 1
            while j < n:
                l = out[j]
                if LABEL.match(l.strip()) or CALLISH.search(l) or l.strip().startswith(('goto ', 'if (')) and 'goto' in l \
                        or l.strip().startswith('REX_LOOP'):
                    break
                ms = VSTORE.match(l)
                if ms and ms.group(3) == v and j > 0 and EA.match(out[j - 1]):
                    raw = f'rraw_{v}'
                    raws.add(raw)
                    if not out[i].strip().startswith('/*raw*/'):
                        ind = m.group(1)
                        out[i] = (f'{ind}/*raw*/ {raw} = simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)); '
                                  f'simde_mm_store_si128((simde__m128i*){v}.u8, REX_BSWAP128({raw}));')
                    ind = ms.group(1)
                    card = ms.group(2) or ''
                    out[j] = (f'{ind}if (s.Note((u32)(ea), 16)) {card}simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), '
                              f'{raw});')
                    counts['raw'] += 1
                    j += 1
                    continue
                if writes_vector(l, v) and not VSTORE.match(l):
                    break
                j += 1
        i += 1
    # fwd: a stack store and its load back with nothing between that can store, call or jump
    for i in range(n):
        m = SSTORE.match(out[i])
        if not m:
            continue
        width, reg_r1, off, src, sty = m.group(2), m.group(3), int(m.group(4)), m.group(5), m.group(6)
        for j in range(i + 1, min(n, i + 40)):
            l = out[j]
            s = l.strip()
            if s == 'if (s.Stopped()) return;':  # the guard's form only (Fast: nothing)
                continue
            if LABEL.match(s) or CALLISH.search(l) or ('goto' in l) or 'return' in l or s.startswith('REX_LOOP'):
                break
            ml = SLOAD.match(l)
            if ml and ml.group(5) == reg_r1 and int(ml.group(6)) == off and ml.group(4) == width:
                dst = ml.group(2)
                val = f'{src}.u32' if width == '32' else f'{src}.u64'
                out[j] = f'{ml.group(1)}{dst}.u64 = {val};  /*fwd*/'
                counts['fwd'] += 1
                break
            if STOREISH.search(l) and not s.startswith('/*raw*/'):
                break
            # the stored register or r1 written: stop
            if re.match(r'^\s*(?:' + re.escape(src) + r'|' + reg_r1 + r')\.\w+ = ', l):
                break
    return out, counts, sorted(raws)


def generate(fn, key, body, src_hash, counts, raws):
    o = []
    w = o.append
    w(f'// GENERATED by tools/ssasr_native_gen.py from app/generated/hot/n_{fn}.h (its f_sub_{fn}, sha1 {src_hash}).')
    w(f'// The native of sub_{fn}: the copy\'s function with {counts["raw"]} vector copies as their bytes and '
      f'{counts["fwd"]} stack loads taking the stored value')
    w('// (the script\'s docstring: why each is exact). Do not edit.')
    w('#pragma once')
    w('')
    real = []
    w(f'namespace banjo::hand::n_{fn} {{')
    w('')
    # with guest calls the native takes the copy's Calls struct (hand.txt calls=); without, localize_native.py calls
    # Native(ctx, base, s) alone
    w('template <class C, class S>' if re.search(r'\bf_\w+\(ctx, base, s[,)]', '\n'.join(body)) else 'template <class S>')
    w('[[gnu::noinline]] bool Native(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s) {')
    w(f'  if (!On({key})) [[unlikely]] return false;')
    w(f'  s.Hand({key}, 1);')
    for r in raws:
        w(f'  [[maybe_unused]] simde__m128i {r};')
    for raw in body:
        line = raw
        if line.strip().startswith('s.Hit('):
            continue
        line = line.replace('if (s.Stopped()) return;', 'if (s.Stopped()) return true;')
        line = line.replace('if (s.Open()) return;', 'if (s.Open()) return true;')
        line = re.sub(r'\breturn; \}', 'return true; }', line)
        line = re.sub(r'REX_CALL_INDIRECT_FUNC_AT\([^,()]+, [^,()]+, ', 'REX_CALL_INDIRECT_FUNC(', line)
        line = re.sub(r'\bf_(\w+)\(ctx, base, s((?:, [\w.]+)*)\)', r'C::\1(ctx, base, s\2)', line)  # (M19: and by reference)
        w(line)
    w('  return true;')
    w('}')
    w('')
    w(f'}}  // namespace banjo::hand::n_{fn}')
    return '\n'.join(o) + '\n'


def callees(body):
    return sorted(set(re.findall(r'\bf_(\w+)\(ctx, base, s(?:, [\w.]+)*\)', '\n'.join(body))))


def written_back(body):
    regs = set()
    for l in body:
        if l.strip().startswith('{ ctx.') and 'return' in l and 'f_sub_' not in l and '(ctx, base)' not in l:
            regs |= set(re.findall(r'ctx\.(\w+) = ', l))
    return sorted(regs, key=lambda r: (r[0], int(re.sub(r'\D', '', r) or 0)))


def externs(body):
    # batch 4 (2026-11-26): and the targets the guarded direct calls of tools/indirect_calls.py name in
    # REX_PROMOTED_CALL (the PC build's check takes the function's address; the Switch's is nothing).
    text = '\n'.join(body)
    return sorted(set(re.findall(r'\b((?:__imp__)?sub_[0-9A-F]{8})\(ctx, base\)', text)) |
                  set(re.findall(r'REX_PROMOTED_CALL\(0x[0-9A-F]+, ((?:__imp__)?sub_[0-9A-F]{8})\)', text)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('funcs', nargs='*')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--list', action='store_true', help='the functions of app/src/cpu/hot/ssasr_natives.txt')
    a = ap.parse_args()
    funcs = [(f, False) for f in a.funcs]
    if a.list and os.path.isfile(LIST):
        for l in open(LIST, encoding='utf-8'):
            l = l.split('#')[0].strip()
            if l:
                parts = l.split()
                funcs.append((parts[0], 'opaque' in parts[1:]))
    written = []
    for item, opaque in funcs:
        fn, _, key = item.partition(':')
        fn = fn.upper().replace('SUB_', '')
        key = key or f'kNative_{fn}'
        body, _ = body_of(fn, a.report)
        new, counts, raws = rewrite(body, opaque)
        src_hash = hashlib.sha1('\n'.join(body).encode()).hexdigest()
        exact = written_back(body)
        if a.report:
            print(f'sub_{fn}: {len(body)} lines; raw vector copies {counts["raw"]}, stack forwards {counts["fwd"]}, '
                  f'opaque constants {counts["opaque"]}; '
                  f'written back {",".join(exact)}; callees {",".join(callees(body))}; real calls {",".join(externs(body))}')
            continue
        hdr = generate(fn, key, new, src_hash, counts, raws)
        if externs(body):
            hdr = hdr.replace('#pragma once\n', '#pragma once\n' + ''.join(f'REX_EXTERN({e});\n' for e in externs(body)), 1)
        path = os.path.join(HOT, f'ssasr_native_{fn}.h')
        old = open(path, encoding='utf-8').read() if os.path.isfile(path) else None
        if old != hdr:
            open(path, 'w', encoding='utf-8', newline='\n').write(hdr)
        written.append(fn)
        print(f'{os.path.relpath(path, ROOT)}: raw {counts["raw"]}, fwd {counts["fwd"]}, opaque {counts["opaque"]}; '
              f'hand.txt: {fn} exact={",".join(exact)}' +
              (f' calls={",".join(c.replace("sub_", "") for c in callees(body))}' if callees(body) else ''))
    if a.list:
        # the headers in one, for app/src/cpu/hot/banjo_hand_natives.h
        allp = os.path.join(HOT, 'ssasr_natives_all.h')
        text = ('// GENERATED by tools/ssasr_native_gen.py --list (app/src/cpu/hot/ssasr_natives.txt).\n#pragma once\n' +
                ''.join(f'#include "hot/ssasr_native_{fn}.h"\n' for fn in written))
        old = open(allp, encoding='utf-8').read() if os.path.isfile(allp) else None
        if old != text:
            open(allp, 'w', encoding='utf-8', newline='\n').write(text)


if __name__ == '__main__':
    main()
