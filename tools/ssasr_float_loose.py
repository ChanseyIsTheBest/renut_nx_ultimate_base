#!/usr/bin/env python3
"""ssasr-nx (2026-11-26, the non-exact batch; the user: "try as many non exact things we need as much performance as
we can get"): the single-precision operations tools/single_floats.py could not prove single, as macros.

single_floats.py turns fadds / fsubs / fmuls / fdivs / fsqrts whose inputs are proven singles into float arithmetic
(exact). The rest stay as the generator wrote them: the double operation, then a rounding to single,
    X.f64 = double(float(A.f64 + B.f64));
which keeps the chain in double registers with an fcvt pair around every operation (1,032 such sites in the hot copies
on 2026-11-26, against 4,656 proven ones). This step writes them as
    X.f64 = REX_FADDS_L(A.f64, B.f64);
whose default is the same expression (exact). With REX_SSASR_SINGLE_LOOSE (CMake SSASR_RELAXED_FLOATS) it is float
arithmetic on the inputs rounded to single first: different only when an input is not a single (a double result
reaching a single operation), where the Xenon rounds the exact result once and this rounds the inputs first. The PC
census (REX_NX_CENSUS 41 / 42, "[census] ssasr non-exact") counts how often an input is not a single. The fused
forms (fmadds and kin, rex/ppc/fma_single.h FmaddsSingle) take the same switch.

Run by tools/post_codegen.py after fnm_sign.py, before the copies (they take the same forms). Idempotent.

  python tools/ssasr_float_loose.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

BINARY = re.compile(r'^(\t+)(\S+)\.f64 = double\(float\((\S+)\.f64 ([-+*/]) (\S+)\.f64\)\);$')
SQRT = re.compile(r'^(\t+)(\S+)\.f64 = double\(float\(sqrt\((\S+)\.f64\)\)\);$')
MACRO = {'+': 'REX_FADDS_L', '-': 'REX_FSUBS_L', '*': 'REX_FMULS_L', '/': 'REX_FDIVS_L'}
MARKER = '// ssasr-nx: the single operations single_floats.py could not prove (tools/ssasr_float_loose.py)'
END = '// (end of tools/ssasr_float_loose.py)\n'
BLOCK = MARKER + '''
#ifndef REX_FADDS_L
#if defined(REX_SSASR_SINGLE_LOOSE)
#define REX_FADDS_L(a, b) double(float(a) + float(b))
#define REX_FSUBS_L(a, b) double(float(a) - float(b))
#define REX_FMULS_L(a, b) double(float(a) * float(b))
#define REX_FDIVS_L(a, b) double(float(a) / float(b))
#define REX_FSQRTS_L(a) double(__builtin_sqrtf(float(a)))
#elif defined(REX_NX_CENSUS_ON)
// batch 4 (2026-11-26): the differential census: the exact result returned, the relaxed one computed beside it and
// compared (rex/ppc/diet_ops.h RexLooseDiffCensus: slots 44-47).
static inline double rex_single_loose_count_(double a, double b, double r, double loose) {
  REX_NX_CENSUS(42);
  if ((a == a && double(float(a)) != a) || (b == b && double(float(b)) != b)) {
    REX_NX_CENSUS(41);
    RexLooseDiffCensus(r, loose);
  }
  return r;
}
#define REX_FADDS_L(a, b) rex_single_loose_count_((a), (b), double(float((a) + (b))), double(float(a) + float(b)))
#define REX_FSUBS_L(a, b) rex_single_loose_count_((a), (b), double(float((a) - (b))), double(float(a) - float(b)))
#define REX_FMULS_L(a, b) rex_single_loose_count_((a), (b), double(float((a) * (b))), double(float(a) * float(b)))
#define REX_FDIVS_L(a, b) rex_single_loose_count_((a), (b), double(float((a) / (b))), double(float(a) / float(b)))
#define REX_FSQRTS_L(a) rex_single_loose_count_((a), 0.0, double(float(sqrt(a))), double(__builtin_sqrtf(float(a))))
#else
#define REX_FADDS_L(a, b) double(float((a) + (b)))
#define REX_FSUBS_L(a, b) double(float((a) - (b)))
#define REX_FMULS_L(a, b) double(float((a) * (b)))
#define REX_FDIVS_L(a, b) double(float((a) / (b)))
#define REX_FSQRTS_L(a) double(float(sqrt(a)))
#endif
#endif
''' + END
ANCHOR = '#define REX_LOAD_U8(x)'


DOUBLE_OP = re.compile(r'^(\t+)(\S+)\.f64 = (\S+)\.f64 ([-+*/]) (\S+)\.f64;$')


def demote_pass(lines):
    """Batch 4 (row 15): a double operation whose result is next rounded to single in place,
        X.f64 = A.f64 * B.f64;  ...  X.f64 = double(float(X.f64));
    with nothing reading X in between, is the single operation double(float(A * B)): written as REX_F*S_L (the same
    expression by default, exact; float arithmetic with SSASR_RELAXED_FLOATS) and the rounding line dropped (a single
    rounded again is itself). 98 such sites in the generated code and the copies on 2026-11-26."""
    n = 0
    for i, l in enumerate(lines):
        m = DOUBLE_OP.match(l)
        if not m:
            continue
        ind, x, a, op, b = m.groups()
        xs = re.escape(x) + r'\.f64'
        for j in range(i + 1, min(len(lines), i + 12)):
            s = lines[j]
            if s.strip().startswith('//') or not re.search(r'(?<![\w.])' + xs, s):
                if s.strip().endswith(':') or 'goto' in s or 'return' in s:
                    break
                continue
            if s.strip() == '%s.f64 = double(float(%s.f64));' % (x, x):
                lines[i] = '%s%s.f64 = %s(%s.f64, %s.f64);  /*demoted*/' % (ind, x, MACRO[op], a, b)
                lines[j] = s[:len(s) - len(s.lstrip())] + '/* rounded above */'
                n += 1
            break
    return n


def rewrite_line(line):
    m = BINARY.match(line)
    if m:
        ind, x, a, op, b = m.groups()
        return '%s%s.f64 = %s(%s.f64, %s.f64);' % (ind, x, MACRO[op], a, b)
    m = SQRT.match(line)
    if m:
        ind, x, a = m.groups()
        return '%s%s.f64 = REX_FSQRTS_L(%s.f64);' % (ind, x, a)
    return None


def patch_pch(path, check):
    text = open(path, encoding='utf-8').read()
    new = text
    if MARKER in new:
        start = new.index(MARKER)
        new = new[:start] + new[new.index(END, start) + len(END):]
    if ANCHOR not in new:
        sys.exit('%s: no %r to put the macros in front of' % (path, ANCHOR))
    new = new.replace(ANCHOR, BLOCK + ANCHOR, 1)
    if new != text and not check:
        open(path, 'w', encoding='utf-8', newline='\n').write(new)
    return new != text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'generated'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if not pchs:
        sys.exit('no *_pch.h in ' + a.gen)
    changed_pch = sum(patch_pch(p, a.check) for p in pchs)
    sites = files = demoted = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        n = demote_pass(lines)
        demoted += n
        for i, l in enumerate(lines):
            if 'double(float(' not in l:
                continue
            r = rewrite_line(l)
            if r is not None:
                lines[i] = r
                n += 1
        if n:
            files += 1
            sites += n
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print('ssasr_float_loose: %d single operations in %d files as REX_F*S_L (%d of them double operations rounded '
          'next, row 15)%s; pch %s' % (sites, files, demoted, ' (check only)' if a.check else '',
                                      'updated' if changed_pch else 'unchanged'))


if __name__ == '__main__':
    main()
