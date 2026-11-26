#!/usr/bin/env python3
"""renut-ultimate (dmc_devil-nx's DMC-12): tools/store_cards.py --raw-cas, the store cards of a game's other modules
(tools/post_codegen.py, module_folders). Checks, on lines of the generator's stwcx. form:
  - without --raw-cas a line is left exactly as it is (the main tree's run is unchanged);
  - with it the compare-and-swap gets its card in front, a stack address keeps none, and a second run changes nothing;
  - restore_text gives the plain line back, and leaves the main tree's REX_GUEST_CAS_C forms to their own rule;
  - post_codegen.module_folders finds a module folder (its *_recomp.N.cpp and one *_pch.h), skips hot/ and others, and
    DMC_MODULE_CARDS=0 finds none.
  python tools/tests/store_cards_raw_cas_test.py
"""
import os
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOLS)
import store_cards  # noqa: E402
import post_codegen  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


CAS_LINE = ('\tctx.cr0.eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea)), '
            'ctx.reserved.u32, __builtin_bswap32(r4.u32));')
CARDED = ('\tctx.cr0.eq = (REX_CARD(ea), __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea)), '
          'ctx.reserved.u32, __builtin_bswap32(r4.u32)));')


def run(lines, raw):
    store_cards.RAW_CAS = raw
    counts = {k: 0 for k in ('REX_STORE_D', 'REX_STORE', 'REX_STVLX', 'REX_STVRX', 'REX_MM_STORE', 'BANJO_HOT',
                             'vector', 'dcbz', 'cas', 'stack')}
    return [store_cards.apply_line(lines, k, counts) for k in range(len(lines))], counts


heap = ['\t// stwcx. r4,0,r3', '\tea = r3.u32;', '\tctx.cr0.lt = 0;', '\tctx.cr0.gt = 0;', CAS_LINE,
        '\tctx.cr0.so = ctx.xer.so;']
out, counts = run(heap, False)
check(out == heap, 'without --raw-cas the lines change')
out, counts = run(heap, True)
check(out[4] == CARDED, 'the card is not in front: %r' % out[4])
check(counts['cas'] == 1 and counts['stack'] == 0, 'counts %r' % counts)
again, _ = run(store_cards.restore_text('\n'.join(out)).split('\n'), True)
check(again == out, 'a second run changes the text')
check(store_cards.restore_text(CARDED) == CAS_LINE, 'restore does not give the builtin back')

stack = ['\t// stwcx. r4,0,r1', '\tea = ctx.r1.u32 + 16;', CAS_LINE]
out, counts = run(stack, True)
check(out == stack and counts['stack'] == 1, 'a stack compare-and-swap was carded')

main = '\tctx.cr0.eq = REX_GUEST_CAS_C(ea, reinterpret_cast<uint32_t*>(REX_RAW_ADDR(ea)), ctx.reserved.u32, x);'
check(store_cards.restore_text(main) == main.replace('REX_GUEST_CAS_C(ea, ', 'REX_GUEST_CAS('),
      'the main tree\'s REX_GUEST_CAS_C is not restored as before')

with tempfile.TemporaryDirectory() as gen:
    mod = os.path.join(gen, 'ntjobcode')
    os.makedirs(mod)
    os.makedirs(os.path.join(gen, 'hot'))
    os.makedirs(os.path.join(gen, 'empty'))
    open(os.path.join(mod, 'renut_recomp.0.cpp'), 'w').write('')
    open(os.path.join(mod, 'renut_pch.h'), 'w').write('')
    open(os.path.join(gen, 'hot', 'renut_recomp.0.cpp'), 'w').write('')
    os.environ.pop('DMC_MODULE_CARDS', None)
    found = post_codegen.module_folders(gen, True)
    check(found == [os.path.normpath(os.path.abspath(mod))], 'module folders %r' % found)
    os.environ['DMC_MODULE_CARDS'] = '0'
    check(post_codegen.module_folders(gen, True) == [], 'DMC_MODULE_CARDS=0 still finds folders')
    os.environ.pop('DMC_MODULE_CARDS', None)

print('store_cards_raw_cas_test:', 'FAILED (%d)' % failures if failures else 'ok')
sys.exit(1 if failures else 0)
