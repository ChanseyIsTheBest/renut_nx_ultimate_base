#!/usr/bin/env python3
"""dmc_devil-nx (renut-ultimate's port of its pass dmc_pwl_fast): tools/deko3d_glsl_opt.py pass_dmc_pwl_fast leaves a
shader as it is without DMC_GLSL_PWL_FAST=1, rewrites the decode block with it, and its fit stays within 0.002 of
shader_common.h's renutPWLGammaToLinear over 0..1 (the bound the pass's comment gives).
Run: python tools/tests/dmc_pwl_fast_check.py (prints "0 failures")."""
import importlib.util
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def load(on):
    if on:
        os.environ['DMC_GLSL_PWL_FAST'] = '1'
    else:
        os.environ.pop('DMC_GLSL_PWL_FAST', None)
    spec = importlib.util.spec_from_file_location('glslopt', os.path.join(HERE, '..', 'deko3d_glsl_opt.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


BLOCK = ('#version 460\n'
         'void main()\n{\n'
         '    float _10 = clamp(_9.x, 0.0, 1.0);\n'
         '    bool _11 = _10 >= 0.3764705955982208251953125;\n'
         '    bool _12 = _10 >= 0.752941191196441650390625;\n'
         '    bool _13 = _10 >= 0.2509804069995880126953125;\n'
         '    float _14 = dk_sel(_11, (dk_sel(_12, 0.0078125, 0.00390625)), (dk_sel(_13, 0.001953125, 0.0009765625)));\n'
         '    float _15 = ((_10 * 261120.0) * _14) + (dk_sel(_11, (dk_sel(_12, (-1024.0), (-256.0))), '
         '(dk_sel(_13, (-64.0), 0.0))));\n'
         '    float _16 = ((_15 + trunc(_15 * _14)) * 0.000977517105638980865478515625);\n'
         '}\n')
fails = 0


def check(what, ok):
    global fails
    if not ok:
        fails += 1
        print('FAIL', what)


off = load(False)
check('off: unchanged', off.pass_dmc_pwl_fast(BLOCK) == BLOCK)
on = load(True)
out = on.pass_dmc_pwl_fast(BLOCK)
check('on: block replaced', 'float _15 = dk_pwl_srgb(_9.x);' in out and 'float _16 = _15;' in out)
check('on: helper added', 'float dk_pwl_srgb(float v)' in out)
check('on: no main, no change', on.pass_dmc_pwl_fast(BLOCK.replace('void main()', 'void other()')) ==
      BLOCK.replace('void main()', 'void other()'))


def exact(v):  # renutPWLGammaToLinear (shader_common.h), as the block computes it
    x = min(max(v, 0.0), 1.0)
    high = x >= 96.0 / 255.0
    scale = (8.0 if x >= 192.0 / 255.0 else 4.0) / 1024.0 if high else (2.0 if x >= 64.0 / 255.0 else 1.0) / 1024.0
    offset = (-1024.0 if x >= 192.0 / 255.0 else -256.0) if high else (-64.0 if x >= 64.0 / 255.0 else 0.0)
    value = x * (255.0 * 1024.0) * scale + offset
    value += math.trunc(value * scale)
    return value / 1023.0


def fit(v):  # dk_pwl_srgb
    x = min(max(v, 0.0), 1.0)
    return max(max(x * 0.24926686, x * 0.49853372 - 0.06256109), max(x * 1.00030547 - 0.25146628,
                                                                      x * 2.01384040 - 1.01384040))


worst = max(abs(exact(i / 4080.0) - fit(i / 4080.0)) for i in range(4081))
check(f'fit within 0.002 (worst {worst:.5f})', worst <= 0.002)
print(f'{fails} failures')
raise SystemExit(1 if fails else 0)
