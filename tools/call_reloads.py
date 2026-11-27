#!/usr/bin/env python3
"""ssasr-nx (2026-11-27, loads batch, the user's idea 4: "knowing what a call can write ... it will be universal"):
a guest load after a call that re-reads an address the same straight-line block read or wrote before the call takes
that value instead, when what the callee can write is known and does not reach the address.

What a function can write (its summary), from the generated text after every earlier step:
  - a store through its stack pointer (r1l / ctx.r1, or an `ea` worked out from it) writes its own frame, below its
    caller's r1: nothing any caller can hold a pointer to;
  - a store through a register whose value is, everywhere in the function, "argument register k at entry plus a
    constant" (every definition agrees: flow-insensitive) writes (k, offset, width);
  - anything else makes the summary ALL: a store through any other register or an `ea` not from the above, a vector
    or block store whose address is aligned or computed, dcbz, an atomic (REX_GUEST_CAS), a barrier, an indirect or
    promoted call, a kernel import, a call of a function an app hook may replace (tools/direct_calls.py), a function
    outside the tree, an MMIO-looking constant (0x7F000000-0x7FFFFFFF);
  - a call composes the callee's summary through the argument registers' values at the call (the nearest definition
    in the same block, a register's value as above); a fixpoint over the call graph; more than 48 entries: ALL.
The forwarding, in each block (from a label to the next): a D-form load `X.f = REX_LOAD_D_T(Y.u32, D);` or store
`REX_STORE_D_T[_C](Y.u32, D, V);` (integer T) records (Y, D, T); the record goes when Y is assigned, when a store may
overlap it (any store but one through Y at a disjoint offset), at anything above that makes a summary ALL, and at a
call whose summary may write it: a write through an argument whose value is Y plus c overlapping [D, D+w), or through
any argument not derived from Y; records whose Y is a volatile ctx register (r0, r3-r12) go at every call. A load of
(Y, D, T) after at least one call since the record becomes REX_CALL_FWD_T(cap, Y.u32, D), the record's value kept in a
local of the function (cap = rex_cfN, set inside the recorded load or store's own expression).
Exactness: the PowerPC orders nothing between a load and another thread's store without a barrier or an atomic, both
of which end every record, so the value read before the call is one the original could have read after it; the
loop headers are labels, so a polling loop reads afresh each time round.
REX_CALL_FWD_OFF (compile time): the original loads. REX_CALL_FWD_VERIFY (with REX_STACK_FWD_VERIFY, the hooked PC
guard builds): the real load too, compared, a mismatch logged (RexCallFwdMismatch).
Runs in tools/post_codegen.py right before reg_liveness.py and the copies, so the copies take the same form;
--restore puts the original text back. renut-ultimate: post_codegen.py runs it only with SSASR_CALL_RELOADS=1 in the environment.
  python tools/call_reloads.py --gen app/generated [--restore] [--check]
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = '// ssasr-nx: guest reloads after calls forwarded (tools/call_reloads.py)'
MACROS = MARKER + '''
#ifndef REX_CALL_FWD_U32
#if defined(REX_CALL_FWD_OFF)
#define REX_CALL_FWD_U8(cap, r, d) REX_LOAD_D_U8(r, d)
#define REX_CALL_FWD_U16(cap, r, d) REX_LOAD_D_U16(r, d)
#define REX_CALL_FWD_U32(cap, r, d) REX_LOAD_D_U32(r, d)
#define REX_CALL_FWD_U64(cap, r, d) REX_LOAD_D_U64(r, d)
#elif defined(REX_CALL_FWD_VERIFY) || defined(REX_STACK_FWD_VERIFY)
extern "C" volatile unsigned long long g_rex_call_fwd_checked;
extern "C" void RexCallFwdMismatch(unsigned address, unsigned long long kept, unsigned long long loaded) noexcept;
#define REX_CALL_FWD_CHECK_(CT, LOAD, cap, r, d) \\
  ({ const CT rex_cfl_ = (CT)LOAD(r, d); ++g_rex_call_fwd_checked; \\
     if (rex_cfl_ != (CT)(cap)) [[unlikely]] RexCallFwdMismatch((u32)(r) + (u32)(d), (unsigned long long)(cap), \\
                                                               (unsigned long long)rex_cfl_); \\
     rex_cfl_; })
#define REX_CALL_FWD_U8(cap, r, d) REX_CALL_FWD_CHECK_(u8, REX_LOAD_D_U8, cap, r, d)
#define REX_CALL_FWD_U16(cap, r, d) REX_CALL_FWD_CHECK_(u16, REX_LOAD_D_U16, cap, r, d)
#define REX_CALL_FWD_U32(cap, r, d) REX_CALL_FWD_CHECK_(u32, REX_LOAD_D_U32, cap, r, d)
#define REX_CALL_FWD_U64(cap, r, d) REX_CALL_FWD_CHECK_(u64, REX_LOAD_D_U64, cap, r, d)
#else
#define REX_CALL_FWD_U8(cap, r, d) ((u8)(cap))
#define REX_CALL_FWD_U16(cap, r, d) ((u16)(cap))
#define REX_CALL_FWD_U32(cap, r, d) ((u32)(cap))
#define REX_CALL_FWD_U64(cap, r, d) ((u64)(cap))
#endif
#endif
'''
DECL_MARK = '  // call_reloads captures'
CTYPE = {'U8': 'u8', 'U16': 'u16', 'U32': 'u32', 'U64': 'u64'}
WIDTH = {'U8': 1, 'U16': 2, 'U32': 4, 'U64': 8, 'F32': 4, 'F64': 8, 'F32ANY': 4, 'F64ANY': 8}
MAX_ENTRIES = 48
ALL = 'ALL'

HEAD = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\) \{$')
LABEL = re.compile(r'^loc_[0-9A-F]{8}:')
VAR = r'(?:ctx\.)?[A-Za-z_]\w*'
LOAD_D = re.compile(r'^\t(' + VAR + r')\.(u64|s64|u32|s32) = REX_LOAD_D_(U8|U16|U32|U64)\((' + VAR + r')\.u32, (-?\d+)\);$')
STORE_D = re.compile(r'REX_STORE_D_(U8|U16|U32|U64|F32ANY|F32|F64ANY|F64)(?:_C)?\((' + VAR + r')\.u32, (-?\d+), ')
STORE_D_LINE = re.compile(r'^\tREX_STORE_D_(U8|U16|U32|U64)(_C)?\((' + VAR + r')\.u32, (-?\d+), (.+)\);$')
STORE_EA = re.compile(r'REX_STORE_(U8|U16|U32|U64|F32ANY|F32|F64ANY|F64)(?:_C)?\(ea\b')
EA_DEF = re.compile(r'^\tea = (.+);$')
ASSIGN = re.compile(r'(?:^\t|;\s*|\{\s*)(' + VAR + r')\.(?:u64|s64|u32|s32|u16|s16|u8|s8)\s*[-+*/|&^]?=(?!=)')
LEAF_STORE = re.compile(r'REX_LEAF_STORE\(\w+, (r\d+)\)')
CALL = re.compile(r'\b(__imp__)?(sub_[0-9A-F]{8})\(ctx, base\)')
KILL_ALL = re.compile(r'REX_GUEST_(?:CAS|\w*SYNC|EIEIO|FENCE)|REX_CALL_INDIRECT|REX_PROMOTED|__imp__(?!sub_)[A-Za-z]|'
                      r'REX_DCBZ|REX_STV[LR]X|REX_MM_STORE|simde_mm_store|_mm_store|memcpy|memset|memmove|'
                      r'__sync|__atomic|REX_LWARX|REX_STWCX|REX_LDARX|REX_STDCX|REX_RESERVE|longjmp|setjmp')
MMIO = re.compile(r'(?<![\w.])(21[3-4]\d{7})(?![\d])')  # 0x7F000000-0x7FFFFFFF as the generator's decimals
COPY = re.compile(r'^(' + VAR + r')\.(?:u64|s64)$')
ADD = re.compile(r'^(' + VAR + r')\.s64 \+ (-?\d+)$')
EA_FORM = re.compile(r'^(?:(-?\d+) \+ (' + VAR + r')\.u32|(' + VAR + r')\.u32(?: \+ (-?\d+))?)$')
VOLATILE = {'ctx.r0'} | {'ctx.r%d' % n for n in range(3, 13)}
STACK_VARS = {'r1l', 'ctx.r1', 'l_r1'}


def functions(path):
    lines = open(path, encoding='utf-8', errors='ignore').read().split('\n')
    out = []
    i = 0
    while i < len(lines):
        m = HEAD.match(lines[i])
        if m:
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            out.append((m.group(1), i, j))
            i = j
        i += 1
    return lines, out


def mmio_value(v):
    return 0x7F000000 <= v < 0x80000000


class Prov:
    """A function's flow-insensitive register values: ('arg', k, c), ('stack', 0, 0) or None (unknown)."""

    def __init__(self, body):
        self.defs = {}
        for line in body:
            for m in ASSIGN.finditer(line):
                rest = line[m.end():].split(';')[0].strip()
                self.defs.setdefault(m.group(1), []).append(rest if line[m.end() - 1] == '=' and
                                                             line[m.end() - 2] == ' ' else '?')
            for m in LEAF_STORE.finditer(line):
                self.defs.setdefault('ctx.' + m.group(1), []).append('?')
        self.memo = {}

    def of(self, var, depth=0):
        if var in STACK_VARS:
            return ('stack', 0, 0)
        if var in self.memo:
            return self.memo[var]
        if depth > 12:
            return None
        self.memo[var] = None  # (a cycle: unknown)
        vals = set()
        m = re.fullmatch(r'ctx\.r(\d+)', var)
        if m and 3 <= int(m.group(1)) <= 10:
            vals.add(('arg', int(m.group(1)), 0))
        elif var not in self.defs:
            return None
        for rhs in self.defs.get(var, []):
            v = self.rhs(rhs, depth)
            if v is None:
                vals = None
                break
            vals.add(v)
        res = next(iter(vals)) if vals and len(vals) == 1 else None
        self.memo[var] = res
        return res

    def rhs(self, rhs, depth):
        m = COPY.match(rhs)
        if m:
            return self.of(m.group(1), depth + 1)
        m = ADD.match(rhs)
        if m:
            b = self.of(m.group(1), depth + 1)
            return (b[0], b[1], b[2] + int(m.group(2))) if b else None
        return None


def ea_value(body, i, prov):
    """The provenance of `ea` at line i: its definition earlier in the same block."""
    for j in range(i - 1, -1, -1):
        line = body[j]
        if LABEL.match(line):
            return None
        m = EA_DEF.match(line)
        if m:
            e = EA_FORM.match(m.group(1).strip())
            if not e:
                return None
            if e.group(2):
                b = prov.of(e.group(2))
                k = int(e.group(1))
            else:
                b = prov.of(e.group(3))
                k = int(e.group(4) or 0)
            return (b[0], b[1], b[2] + k) if b else None
    return None


def arg_value_at(body, i, k, prov, assigned):
    """ctx.r<k>'s provenance at the call on line i: its nearest definition in the block, or its entry value if the
    function never assigns it."""
    var = 'ctx.r%d' % k
    for j in range(i - 1, -1, -1):
        line = body[j]
        if LABEL.match(line):
            break
        for m in ASSIGN.finditer(line):
            if m.group(1) == var:
                rest = line[m.end():].split(';')[0].strip()
                return prov.rhs(rest, 0) if line[m.end() - 2] == ' ' else None
        if CALL.search(line):
            break  # (a call between may change it)
    if var not in assigned:
        return ('arg', k, 0)
    return None


def summarise(name, body, hooked):
    """(own writes or ALL, calls: [(callee, {k: provenance}, direct)])"""
    prov = Prov(body)
    assigned = set(prov.defs)
    writes = set()
    calls = []
    for i, line in enumerate(body):
        if KILL_ALL.search(line) or MMIO.search(line):
            return ALL, []
        for m in STORE_D.finditer(line):
            v = prov.of(m.group(2))
            if v is None:
                return ALL, []
            if v[0] == 'arg':
                writes.add((v[1], v[2] + int(m.group(3)), WIDTH[m.group(1)]))
        for m in STORE_EA.finditer(line):
            v = ea_value(body, i, prov)
            if v is None:
                return ALL, []
            if v[0] == 'arg':
                writes.add((v[1], v[2], WIDTH[m.group(1)]))
        if 'REX_STORE' in line and not STORE_D.search(line) and not STORE_EA.search(line):
            return ALL, []
        for m in CALL.finditer(line):
            callee = m.group(2)
            if not m.group(1) and callee in hooked:
                return ALL, []
            args = {k: arg_value_at(body, i, k, prov, assigned) for k in range(3, 11)}
            calls.append((callee, args))
    return writes, calls


def compose(callee_sum, args):
    if callee_sum == ALL:
        return ALL
    out = set()
    for k, d, w in callee_sum:
        a = args.get(k)
        if a is None:
            return ALL
        if a[0] == 'stack':
            continue  # into the caller's own frame
        out.add((a[1], a[2] + d, w))
    return out


def overlap(a, aw, b, bw):
    return a < b + bw and b < a + aw


def forward(body, summaries, hooked, counter, why=None):
    """The function's body with its reloads after calls forwarded; (new body, forwarded count, captures)."""
    if any(MMIO.search(l) for l in body):
        return body, 0, []
    # pass 1: which recorded lines feed a forwarded reload
    records = {}      # key (Y, D, T) -> [line of the record, crossed a call]
    killed = {}       # (--why) key -> reason it went
    argvals = {}      # ctx.rK -> (var, c) set in this block
    uses = {}         # reload line -> record line
    feeders = set()
    for i, line in enumerate(body):
        if LABEL.match(line):
            records.clear()
            argvals.clear()
            killed.clear()
            continue
        if KILL_ALL.search(line):
            for key in records:
                killed[key] = 'barrier / atomic / indirect / kernel / block store'
            records.clear()
            argvals.clear()
            continue
        lm = LOAD_D.match(line)
        if lm:
            key = (lm.group(4), int(lm.group(5)), lm.group(3))
            if key in records and records[key][1] and lm.group(4) != lm.group(1):
                uses[i] = records[key][0]
                feeders.add(records[key][0])
            elif why is not None and key not in records and key in killed:
                why[killed.pop(key)] += 1
        # stores: kill what may overlap, then record an integer D-form store
        sm = STORE_D_LINE.match(line)
        if STORE_D.search(line) or STORE_EA.search(line) or ('REX_STORE' in line):
            if sm:
                y, d, t = sm.group(3), int(sm.group(4)), sm.group(1)
                for key in list(records):
                    if key[0] != y or overlap(key[1], WIDTH[key[2]], d, WIDTH[t]):
                        killed[key] = 'a store through another register' if key[0] != y else 'a store over it'
                        del records[key]
            else:
                for key in records:
                    killed[key] = 'a store through ea or another form'
                records.clear()
        # calls
        cm = CALL.search(line)
        if cm:
            callee = cm.group(2)
            s = summaries.get(callee, ALL) if (cm.group(1) or callee not in hooked) else ALL
            if s == ALL:
                for key in records:
                    killed[key] = 'a call that may write anything'
                records.clear()
            else:
                for key in list(records):
                    y, d, t = key
                    if y in VOLATILE:
                        killed[key] = 'its base is a volatile register (the call changes it)'
                        del records[key]
                        continue
                    for k, wd, ww in s:
                        a = argvals.get('ctx.r%d' % k)
                        if a is None or a[0] != y or overlap(a[1] + wd, ww, d, WIDTH[t]):
                            killed[key] = ('a call writing through an unknown argument' if a is None else
                                           'a call writing through another pointer' if a[0] != y else
                                           'a call writing over it')
                            del records[key]
                            break
                for key in records:
                    records[key][1] = True
            argvals = {k: v for k, v in argvals.items() if k not in VOLATILE}
        # assignments: records based on the assigned register go, argument values follow
        for m in ASSIGN.finditer(line):
            var = m.group(1)
            for key in [k for k in records if k[0] == var]:
                killed[key] = 'its base register reassigned'
                del records[key]
            argvals = {k: v for k, v in argvals.items() if v[0] != var}
            if var in VOLATILE and var.startswith('ctx.r'):
                rest = line[m.end():].split(';')[0].strip()
                c = COPY.match(rest)
                a = ADD.match(rest)
                if c:
                    argvals[var] = (c.group(1), 0)
                elif a:
                    argvals[var] = (a.group(1), int(a.group(2)))
                else:
                    argvals.pop(var, None)
        for m in LEAF_STORE.finditer(line):
            var = 'ctx.' + m.group(1)
            for key in [k for k in records if k[0] == var]:
                del records[key]
            argvals.pop(var, None)
        # record after the line's own effects (a load into its own base register records nothing; a forwarded load
        # keeps the record it took its value from)
        if lm and lm.group(1) != lm.group(4) and i not in uses:
            records[(lm.group(4), int(lm.group(5)), lm.group(3))] = [i, False]
        if sm:
            records[(sm.group(3), int(sm.group(4)), sm.group(1))] = [i, False]
    if not uses:
        return body, 0, []
    # pass 2: rewrite
    cap = {}
    out = list(body)
    for r in sorted(feeders):
        lm = LOAD_D.match(body[r])
        n = counter[0]
        counter[0] += 1
        if lm:
            t = lm.group(3)
            cap[r] = ('rex_cf%d' % n, t)
            out[r] = '\t%s.%s = (rex_cf%d = REX_LOAD_D_%s(%s.u32, %s));' % (lm.group(1), lm.group(2), n, t, lm.group(4),
                                                                         lm.group(5))
        else:
            sm = STORE_D_LINE.match(body[r])
            t = sm.group(1)
            cap[r] = ('rex_cf%d' % n, t)
            out[r] = '\tREX_STORE_D_%s%s(%s.u32, %s, (rex_cf%d = (%s)(%s)));' % (t, sm.group(2) or '', sm.group(3),
                                                                                 sm.group(4), n, CTYPE[t], sm.group(5))
    for i, r in uses.items():
        lm = LOAD_D.match(body[i])
        c, t = cap[r]
        if t != lm.group(3):
            continue
        out[i] = '\t%s.%s = REX_CALL_FWD_%s(%s, %s.u32, %s);' % (lm.group(1), lm.group(2), t, c, lm.group(4),
                                                                 lm.group(5))
    return out, len(uses), sorted(set(cap.values()))


RESTORE_LOAD = re.compile(r'\(rex_cf\d+ = (REX_LOAD_D_\w+\([^()]*\))\)')
RESTORE_STORE = re.compile(r'^(\tREX_STORE_D_\w+\([^,]+, -?\d+, )\(rex_cf\d+ = \(\w+\)\((.*)\)\)\);$')
RESTORE_FWD = re.compile(r'REX_CALL_FWD_(\w+)\(rex_cf\d+, ([^,]+), (-?\d+)\)')


def restore_text(text):
    out = []
    for line in text.split('\n'):
        if line.endswith(DECL_MARK):
            continue
        m = RESTORE_STORE.match(line)
        if m:
            line = m.group(1) + m.group(2) + ');'
        line = RESTORE_LOAD.sub(r'\1', line)
        line = RESTORE_FWD.sub(r'REX_LOAD_D_\1(\2, \3)', line)
        out.append(line)
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--restore', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--why', action='store_true', help='with --check: why the re-reads after calls were not forwarded')
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')))
    pch = glob.glob(os.path.join(a.gen, '*_pch.h'))[0]
    if a.restore:
        n = 0
        for p in files:
            t = open(p, encoding='utf-8', newline='').read()
            if 'rex_cf' in t:
                open(p, 'w', encoding='utf-8', newline='').write(restore_text(t))
                n += 1
        print('call_reloads --restore: %d files back to the plain loads' % n)
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import direct_calls
    hooked = {h for h in direct_calls.hooked(a.gen)}
    # 1. summaries
    texts = {}
    own = {}
    for p in files:
        lines, funcs = functions(p)
        texts[p] = (lines, funcs)
        for name, i, j in funcs:
            own[name] = summarise(name, lines[i + 1:j], hooked)
    summaries = {}
    for name, (w, calls) in own.items():
        summaries[name] = ALL if w == ALL else set(w)
    changed = True
    rounds = 0
    while changed and rounds < 50:
        changed = False
        rounds += 1
        for name, (w, calls) in own.items():
            if summaries[name] == ALL:
                continue
            cur = set(summaries[name])
            for callee, args in calls:
                c = compose(summaries.get(callee, ALL), args)
                if c == ALL:
                    cur = ALL
                    break
                cur |= c
            if cur != ALL and len(cur) > MAX_ENTRIES:
                cur = ALL
            if cur != summaries[name]:
                summaries[name] = cur
                changed = True
    n_all = sum(1 for s in summaries.values() if s == ALL)
    n_free = sum(1 for s in summaries.values() if s != ALL and not s)
    print('call_reloads: %d functions: %d write nothing but their stack, %d through their arguments only, %d anything '
          '(%d rounds)' % (len(summaries), n_free, len(summaries) - n_all - n_free, n_all, rounds))
    # 2. forwarding
    total = 0
    files_changed = 0
    counter = [0]
    import collections
    why = collections.Counter() if a.why else None
    for p in files:
        lines, funcs = texts[p]
        new = list(lines)
        changed_here = False
        for name, i, j in reversed(funcs):
            counter[0] = 0
            body = lines[i + 1:j]
            out, n, caps = forward(body, summaries, hooked, counter, why)
            if n:
                total += n
                changed_here = True
                decl = []
                for t in ('U8', 'U16', 'U32', 'U64'):
                    names = [c for c, tt in caps if tt == t]
                    if names:
                        decl.append('\t%s %s;%s' % (CTYPE[t], ', '.join(names), DECL_MARK))
                # the declarations right after the prologue line
                k = next((x for x, l in enumerate(out) if 'REX_FUNC_PROLOGUE' in l), -1)
                out = out[:k + 1] + decl + out[k + 1:]
                new[i + 1:j] = out
        if changed_here:
            files_changed += 1
            if not a.check:
                open(p, 'w', encoding='utf-8', newline='').write('\n'.join(new))
    if why is not None:
        print('call_reloads --why: re-reads of an address read or written earlier in the block, not forwarded:')
        for r, c in why.most_common():
            print('   %6d  %s' % (c, r))
    print('call_reloads: %d guest reloads after calls forwarded, in %d files%s' % (total, files_changed,
                                                                                  ' (check: nothing written)' if a.check else ''))
    if not a.check:
        t = open(pch, encoding='utf-8', newline='').read()
        if MARKER not in t:
            nl = '\r\n' if '\r\n' in t else '\n'
            t = t.rstrip() + nl + nl + MACROS.replace('\n', nl) + nl  # (macros: where they sit does not matter)
            open(pch, 'w', encoding='utf-8', newline='').write(t)


if __name__ == '__main__':
    main()
