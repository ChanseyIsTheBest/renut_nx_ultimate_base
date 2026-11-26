#!/usr/bin/env python3
"""dmc_devil-nx (renut-ultimate's port of its samplerk forms): tools/deko3d_glsl_opt.py pass_samplerk on a GLSL piece with
a sampler index + k read masked and unmasked, with DMC_GLSL_SAMPLERK_RAW unset (on) and 0 (the base's pass), and the
value check behind the rewrite ((word + k) >> s == word >> s for every word the renderer writes: bits 0-12 zero,
k < 2^13). Run: python tools/tests/dmc_samplerk_raw_check.py (prints "0 failures")."""
import importlib.util
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))


def load(raw):
    if raw is None:
        os.environ.pop('DMC_GLSL_SAMPLERK_RAW', None)
    else:
        os.environ['DMC_GLSL_SAMPLERK_RAW'] = raw
    spec = importlib.util.spec_from_file_location('glslopt', os.path.join(HERE, '..', 'deko3d_glsl_opt.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


SRC = ('void main()\n{\n'
       '    uint _101 = g_UboShared.v[24u].z + 1u;\n'
       '    uint _102 = _101 >> 16u;\n'
       '    uint _103 = _101 >> 13u;\n'
       '    uint _104 = (_101 >> 16u) & 15u;\n'
       '    uint _105 = _102 & 15u;\n'
       '}\n')
fails = 0


def check(what, ok):
    global fails
    if not ok:
        fails += 1
        print('FAIL', what)


on = load(None)
out = on.pass_samplerk(SRC)
check('raw signs read rewritten', f'{on.SK_MARK}\n    uint _102 = g_UboShared.v[24u].z >> 16u;' in out)
check('raw gamma read rewritten', f'{on.SK_MARK}\n    uint _103 = g_UboShared.v[24u].z >> 13u;' in out)
check('masked read as before', f'{on.SK_MARK}\n    uint _104 = (g_UboShared.v[24u].z >> 16u) & 15u;' in out)
check('other lines kept', '    uint _105 = _102 & 15u;' in out and '    uint _101 = g_UboShared.v[24u].z + 1u;' in out)
off = load('0')
out0 = off.pass_samplerk(SRC)
check('off: raw reads untouched', '    uint _102 = _101 >> 16u;' in out0 and '    uint _103 = _101 >> 13u;' in out0)
check('off: masked read rewritten', '    uint _104 = (g_UboShared.v[24u].z >> 16u) & 15u;' in out0)
rng = random.Random(51)
for _ in range(200000):
    word = rng.getrandbits(32) & ~0x1FFF  # (slot << 20) | gamma << 16 | biased << 13: bits 0-12 zero
    k = rng.randrange(1, 1 << 13)
    for s in (13, 16):
        if ((word + k) & 0xFFFFFFFF) >> s != word >> s:
            fails += 1
print(f'{fails} failures')
raise SystemExit(1 if fails else 0)
