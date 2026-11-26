#!/usr/bin/env python3
"""ilomilo-nx (2026-11-25): the next batch of eligible machine copies (the base's docs/optimising-functions.md, "What to
do next" 1 and 2): functions no profile sampled, chosen by tools/switch_hot_bulk.py's own rules, added to the bulk list.

    python tools/hot_eligible_batch.py --count 300 [--write app/src/cpu/hot/localized_bulk.txt] [--dry-run]

Every generated function, in this order: the direct callees of what is already copied (the list and the hand
natives), then their callers, then every other one by address. A function goes on the batch unless switch_hot_bulk.py would leave it out:
already a native, a tiny leaf (at most --tiny instructions, no calls), small (under --min-instructions: with no
samples it never has the 50 that keep a small one), an app or SDK source mentions it, threaded dispatch
(switch_goto.py), or localize_native.fits() refuses it (a raw guest access is let through as switch_hot_bulk.py
does). The lines go to the end of the list with "eligible batch" in place of the samples; tools/post_codegen.py then
makes the copies (localize_native.py --bulk). A copy no run reaches is not checked by the guard: the batch's verify run
says how many were reached.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--count', type=int, default=300)
    ap.add_argument('--write', default=None, help='the bulk list (default: localize_native.BULK)')
    ap.add_argument('--tiny', type=int, default=12)
    ap.add_argument('--min-instructions', type=int, default=40)
    ap.add_argument('--label', default='eligible batch')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    import localize_native as ln
    import store_cards
    path = a.write or ln.BULK
    index = ln.Index(ln.GEN)
    listed = [e[0] for e in ln.list_entries(ln.LIST)]
    hand = ln.hand_entries()
    hooks = ln.Hooks(listed)
    sizes = ln.addresses(ln.GEN)
    text = open(path, encoding='utf-8').read()
    bulk = set('sub_' + m for m in re.findall(r'^([0-9A-F]{8})\b', text, re.M))

    def raw_ok(sub):
        for _, l in index.body(sub):
            l = store_cards.restore_text(l)
            if 'REX_RAW_ADDR(' in l and not (ln.RAW_VECTOR_STORE.match(l) or ln.RAW_MEMSET.match(l) or
                                             ln.RAW_LOAD.match(l) or
                                             (ln.ATOMICS and (ln.ATOMIC_LOAD.match(l) or ln.ATOMIC_CAS.match(l)))):
                return False
        return True

    post = open(os.path.join(ROOT, 'tools', 'post_codegen.py'), encoding='utf-8').read()
    threaded = set(re.findall(r'sub_[0-9A-F]{8}', post[post.find("tool('switch_goto.py'), '--gen', gen, '--thread'"):][:300]))

    def why_not(sub):
        f = index.funcs.get(sub)
        if f is None:
            return 'not a generated function'
        if sub in listed or sub in hand or sub in bulk:
            return 'already a native'
        n = sizes.get(sub, (0, 0))[1] // 4
        if not f['callees'] and not f['unknown'] and n <= a.tiny:
            return 'tiny leaf'
        if a.min_instructions and n < a.min_instructions:
            return 'small'
        if hooks.replaced(sub):
            return 'an app or SDK source mentions it'
        if sub in threaded:
            return 'threaded dispatch'
        why = ln.fits(index, hooks, sub)
        if why == 'raw guest access' and raw_ok(sub):
            why = None
        return why

    copied = set(listed) | set(hand) | bulk
    callees = []
    for sub in sorted(copied):
        for c in (index.funcs.get(sub) or {}).get('callees', []):
            if c not in copied and c not in callees:
                callees.append(c)
    # Then the callers of what is copied (a function that calls a hot one tends to run often itself), then the rest.
    callers = [s for s in sorted(index.funcs)
               if s not in copied and s not in callees and copied & set(index.funcs[s].get('callees', []))]
    first = set(callees) | set(callers)
    order = callees + callers + [s for s in sorted(index.funcs) if s not in first]
    chosen, left, from_callees = [], {}, 0
    for sub in order:
        if len(chosen) >= a.count:
            break
        why = why_not(sub)
        if why:
            key = why.split(':')[0][:40]
            left[key] = left.get(key, 0) + 1
            continue
        chosen.append(sub)
        if sub in callees:
            from_callees += 1
    eligible_total = sum(1 for s in index.funcs if not why_not(s)) if a.dry_run else None
    print(f'hot_eligible_batch: {len(chosen)} chosen ({from_callees} callees, {sum(1 for s in chosen if s in set(callers))} callers of copies first); '
          f'passed over on the way: ' + ', '.join(f'{k} {v}' for k, v in sorted(left.items(), key=lambda x: -x[1])))
    if eligible_total is not None:
        print(f'hot_eligible_batch: {eligible_total} eligible functions in all, {len(index.funcs)} generated')
    if a.dry_run:
        return
    rows = [f'{s[4:]} {a.label}, {sizes.get(s, (0, 0))[1] // 4} instructions, '
            f'{len(index.funcs[s]["callees"])} direct callees' for s in chosen]
    with open(path, 'a', encoding='utf-8', newline='\n') as out:
        if not text.endswith('\n'):
            out.write('\n')
        out.write('\n'.join(rows) + '\n')
    print(f'hot_eligible_batch: {len(rows)} lines added to {os.path.relpath(path, ROOT)}')


if __name__ == '__main__':
    main()
