#!/usr/bin/env python3
"""renut-ultimate (2026-11-27): tools/state_block_scan.py against a small made-up generated tree: a capture-shaped
function (all six marks), an apply-shaped one (five: no 26 loop), a frame-timing one with a 26 and the type compares
(three), a 4,096 in another register (no mark), a capture inside a function of 900 more instructions (its score kept,
not a candidate under --max-n), one outside the range, and the exit codes (0 with a candidate, 1 without, 2 on a bad argument or an empty
range). Exit 0: every check passed.
"""
import contextlib
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import state_block_scan as sbs  # noqa: E402


def func(name, body):
    return f'DEFINE_REX_FUNC({name}) {{\n' + ''.join(f'\t// {i}\n\tstub();\n' for i in body) + '}\n\n'


CAPTURE = ['mflr r12', 'li r28,26', 'bl 0x82173dc8', 'cmplwi cr6,r26,1', 'cmplwi cr6,r28,16', 'lhzx r11,r11,r27',
           'mtctr r11', 'bctrl ', 'bctrl ', 'li r5,4096', 'cmplwi cr6,r26,3', 'blr']
APPLY = ['mflr r12', 'cmplwi cr6,r26,1', 'cmplwi cr6,r28,16', 'lhzx r11,r11,r27', 'bctrl ', 'bctrl ', 'bctrl ',
         'li r5,4096', 'cmplwi r26,3', 'blr']
TIMING = ['mflr r12', 'mftb r11', 'li r11,26', 'cmplwi cr6,r11,1', 'cmplwi cr6,r11,3', 'bctrl ', 'bctrl ', 'blr']
WIDE = ['li r6,4096', 'cmplwi cr6,r26,1', 'cmplwi cr6,r28,16', 'lhzx r11,r11,r27', 'bctrl ', 'bctrl ', 'cmplwi r26,3']
BIG = CAPTURE[:-1] + ['nop'] * 900 + ['blr']


def run(args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = sbs.main(args)
    return code, out.getvalue()


def main():
    fails = 0

    def check(what, ok):
        nonlocal fails
        if not ok:
            fails += 1
            print('FAIL', what)

    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, 'renut_recomp.0.cpp'), 'w') as f:
            f.write(func('sub_82239650', CAPTURE) + func('sub_82239A28', APPLY) + func('sub_824E5718', TIMING) +
                    func('sub_82300000', WIDE) + func('sub_82400000', BIG))
        with open(os.path.join(d, 'renut_recomp.1.cpp'), 'w') as f:
            f.write(func('sub_83000010', CAPTURE))  # outside the default range
        found = {name: (score, m, n) for score, name, m, n in sbs.scan(d, 0x82000000, 0x83000000)}
        check('capture scores 6', found.get('sub_82239650', (0,))[0] == 6)
        check('apply scores 5', found.get('sub_82239A28', (0,))[0] == 5 and not found['sub_82239A28'][1]['li26'])
        check('timing scores 3', found.get('sub_824E5718', (0,))[0] == 3)
        check('instruction count', found.get('sub_82239650', (0, 0, 0))[2] == len(CAPTURE))
        check('range end excluded', 'sub_83000010' not in found)
        check('4096 in another register is no mark', found.get('sub_82300000', (0, {'li4096': True}))[1]['li4096'] is False)
        check('a big function keeps its score', found.get('sub_82400000', (0,))[0] == 6)
        code, out = run([d])
        check('exit 0 with candidates', code == 0 and out.count('<- candidate') == 2)
        check('a big function is not a candidate', 'sub_82400000 n=' in out and 'over --max-n 800' in out)
        code, out = run([d, '--min', '6'])
        check('--min 6: one candidate', code == 0 and out.count('<- candidate') == 1)
        code, out = run([d, '--max-n', '2000'])
        check('--max-n 2000: the big one counts', code == 0 and out.count('<- candidate') == 3)
        check('exit 2: --max-n 0', run([d, '--max-n', '0'])[0] == 2)
        code, _ = run([d, '--lo', '824E0000', '--hi', '824F0000'])
        check('exit 1 without a candidate', code == 1)
        check('exit 2: empty range', run([d, '--lo', '84000000', '--hi', '85000000'])[0] == 2)
        check('exit 2: bad hex', run([d, '--lo', 'zz'])[0] == 2)
        check('exit 2: --min 7', run([d, '--min', '7'])[0] == 2)
        check('exit 2: lo >= hi', run([d, '--lo', '83000000', '--hi', '82000000'])[0] == 2)
        check('exit 2: no folder', run([os.path.join(d, 'none')])[0] == 2)
    print('state_block_scan_test:', 'ALL OK' if not fails else f'{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
