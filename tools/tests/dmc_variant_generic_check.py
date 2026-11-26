#!/usr/bin/env python3
"""dmc_devil-nx (renut-ultimate's port of its variant cache change): tools/deko3d_variant_cache.py's generic_of and
target_bias_927 against render/pipeline.cpp's rules for the generic pipeline GetPipelineSlow draws with (the reserved
word kept, sasrt_words[7] & 0xFF010000 kept with asura_renut_generic_keeps_clamps). No game data, no uam.
Run: python tools/tests/dmc_variant_generic_check.py (prints "0 failures")."""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('vc', os.path.join(HERE, '..', 'deko3d_variant_cache.py'))
vc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vc)

fails = 0


def check(what, got, want):
    global fails
    if got != want:
        fails += 1
        print(f'FAIL {what}: {got} != {want}')


tail = {vc.FLAG_TESS: 0, vc.FLAG_GS: 0, vc.FLAG_DEPTH_WRITES: 1, 901: 8}
# 927: 1 plain, 2 with the fixed16 bit, 1 | wide << 4 with wide-alpha targets 0 and 2, fixed16 first
check('927 plain', vc.target_bias_927(0), 1)
check('927 fixed16', vc.target_bias_927(0x10000), 2)
check('927 wide', vc.target_bias_927(0x05000000), 1 | (5 << 4))
check('927 fixed16 and wide', vc.target_bias_927(0x05010000), 2)
# a gamma target 0 (924 = 1) with bias 5 on target 0 (925 = 5): reserved bits 4-7 and 8+
reserved = (1 << 4) | (5 << 8)
g = vc.generic_of(tail, 1, reserved, False, 0, kept7=0xFF010000)
check('gamma and bias', {k: g[k] for k in (924, 925, 927)}, {924: 1, 925: 5, 927: 1})
# a 7e3 target 1 (990 = 2, bit 29) kept with the clamps, dropped without them
g = vc.generic_of(tail, 1, 0, False, 0x20000000, kept7=0xFF010000)
check('7e3 kept', g.get(990), 2)
g = vc.generic_of(tail, 1, 0, False, 0x20000000, kept7=0)
check('7e3 not kept', g.get(990), None)
# no user clip plane: 929 = 0 in the generic (the reserved word is kept)
g = vc.generic_of(tail, 0, 0, True, 0, kept7=0xFF010000)
check('no clip', g.get(929), 0)
# the tail itself is not changed
check('tail untouched', tail, {vc.FLAG_TESS: 0, vc.FLAG_GS: 0, vc.FLAG_DEPTH_WRITES: 1, 901: 8})
print(f'{fails} failures')
raise SystemExit(1 if fails else 0)
