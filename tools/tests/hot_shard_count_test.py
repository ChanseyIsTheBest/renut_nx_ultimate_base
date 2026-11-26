#!/usr/bin/env python3
"""renut-ultimate (pokersmash-nx's 60 shards): tools/localize_native.py's shard_count() reads app/CMakeLists.txt's
`set(BANJO_HOT_SHARD_COUNT N)`, the one place the number is set: the base's 12, another number, no line (12, the count
before), a commented-out line (not taken), and a number out of 1-256 (refused). Also that the real app/CMakeLists.txt
and every shard file it names agree with it. Reads files only.
  python tools/tests/hot_shard_count_test.py
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import localize_native as ln  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def count_for(text):
    with tempfile.TemporaryDirectory() as d:
        if text is not None:
            with open(os.path.join(d, 'CMakeLists.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
                f.write(text)
        saved = ln.APP
        ln.APP = d
        try:
            return ln.shard_count()
        finally:
            ln.APP = saved


check(count_for('set(RENUT_GAME_NAME "X")\nset(BANJO_HOT_SHARD_COUNT 12)\n') == 12, 'the base\'s 12')
check(count_for('  set(BANJO_HOT_SHARD_COUNT 60)\nmath(EXPR x "1")\n') == 60, 'a port\'s 60')
check(count_for('set(RENUT_GAME_NAME "X")\n') == 12, 'no line: 12')
check(count_for('# set(BANJO_HOT_SHARD_COUNT 60)\n') == 12, 'a commented-out line is not taken')
check(count_for(None) == 12, 'no CMakeLists.txt: 12')
try:
    count_for('set(BANJO_HOT_SHARD_COUNT 0)\n')
    check(False, '0 shards accepted')
except SystemExit:
    pass
try:
    count_for('set(BANJO_HOT_SHARD_COUNT 999)\n')
    check(False, '999 shards accepted')
except SystemExit:
    pass

# The tree itself: the tool's number, and a shard file for each.
n = ln.shard_count()
check(1 <= n <= 256, f'app/CMakeLists.txt: {n}')
missing = [k for k in range(n) if not os.path.isfile(os.path.join(ROOT, 'app', 'src', 'cpu', f'banjo_hot_shard_{k}.cpp'))]
check(not missing, f'shard files missing for {missing}')

print('hot_shard_count_test:', 'ALL OK' if not failures else f'{failures} FAILED')
sys.exit(1 if failures else 0)
