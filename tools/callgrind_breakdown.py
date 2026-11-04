#!/usr/bin/env python3
"""Break a callgrind profile of lob_latency into components.

Usage: callgrind_breakdown.py CALLGRIND_OUT MEASURED_OPS

Input is a callgrind.out (text) recorded with
  valgrind --tool=callgrind --collect-atstart=no --cache-sim=yes --branch-sim=yes
on a build with -DLOB_CALLGRIND, so only the order book's work is collected.

What is MEASURED (read straight from the profile): instruction counts, simulated cache misses,
simulated branch mispredicts, call counts, per-function self and inclusive cost, and self cost per
source file (which separates inlined std::map / unordered_map / vector code).
What is ESTIMATED: the 50/50 split of resting-order allocator work between the Order object and
the unordered_map node, the split of one mixed delete bucket between vector<Trade> buffers and
map nodes (by call counts), and every "est. cycles" number (a simple cost model, not hardware).
"""
import collections
import re
import sys

EV = "Ir Dr Dw I1mr D1mr D1mw ILmr DLmr DLmw Bc Bcm Bi Bim".split()
IX = {e: i for i, e in enumerate(EV)}


def parse(path):
    fnname, fname = {}, {}

    def res(table, tok):
        m = re.match(r'\((\d+)\)\s*(.*)', tok)
        if not m:
            return tok
        i, nm = m.group(1), m.group(2)
        if nm:
            table[i] = nm
        return table.get(i, '?' + i)

    self_fn = collections.defaultdict(lambda: [0] * len(EV))
    self_fnfile = collections.defaultdict(lambda: [0] * len(EV))
    edges = collections.defaultdict(lambda: [0, [0] * len(EV)])  # (caller, callee) -> calls, inclusive
    cur_fn = cur_file = cfn = None
    pending = None
    for line in open(path):
        line = line.rstrip('\n')
        if not line:
            continue
        if line[0].isalpha():
            key, _, rest = line.partition('=')
            if key == 'fn':
                cur_fn = res(fnname, rest)
            elif key in ('fl', 'fi', 'fe'):
                cur_file = res(fname, rest)
            elif key == 'cfn':
                cfn = res(fnname, rest)
            elif key in ('cfi', 'cfl'):
                res(fname, rest)
            elif key == 'calls':
                pending = int(rest.split()[0])
        elif line[0] in '0123456789+-*':
            costs = [int(x) for x in line.split()[1:1 + len(EV)]]
            costs += [0] * (len(EV) - len(costs))
            if pending is not None:
                e = edges[(cur_fn, cfn)]
                e[0] += pending
                for i, v in enumerate(costs):
                    e[1][i] += v
                pending = None
            else:
                for i, v in enumerate(costs):
                    self_fn[cur_fn][i] += v
                    self_fnfile[(cur_fn, cur_file)][i] += v
    return self_fn, self_fnfile, edges


def short(n, width=78):
    n = re.sub(r'std::map<long, lob::OrderBook::PriceLevel, std::(\w+)<long>.*?>(?=::)', r'map<\1>', n)
    n = re.sub(r'std::_Hashtable<.*?>(?=::)', 'unordered_map', n)
    return n[:width]


ALLOC_FN = {'malloc', 'free', '_int_malloc', '_int_free', '_int_free_merge_chunk',
            '_int_free_create_chunk', 'malloc_consolidate', 'operator new(unsigned long)',
            'operator delete(void*)', 'operator delete(void*, unsigned long)'}


def category(fn, fl):
    f = fl or ''
    if fn in ALLOC_FN or fn.startswith('0x') or 'malloc/' in f:
        return 'heap allocator (malloc/free/operator new/delete)'
    if fn.startswith('std::_Rb_tree_') or f.endswith(('stl_tree.h', 'stl_map.h')):
        return 'std::map (price levels)'
    if '_Hashtable' in fn or '_Prime_rehash' in fn or \
            f.endswith(('hashtable.h', 'hashtable_policy.h', 'unordered_map.h', 'functional_hash.h')):
        return 'unordered_map (id index)'
    if f.endswith(('stl_vector.h', 'vector.tcc', 'stl_uninitialized.h')):
        return 'vector<Trade> logic'
    if f.endswith(('new_allocator.h', 'alloc_traits.h', 'unique_ptr.h')):
        return 'allocator/unique_ptr glue'
    if f.endswith(('order_book.cpp', 'order_book.hpp')):
        return 'matching loop + book logic'
    if f.endswith('latency_bench.cpp'):
        return 'harness (apply wrapper, loop, checksum)'
    return 'other (std::min/copy, memcpy, misc)'


# Cost model for the ESTIMATED cycle shares. Relative shares only; not calibrated to hardware.
BASE_CPI, D1_PENALTY, LL_PENALTY, MISPREDICT = 0.35, 14, 250, 15


def cycles(v):
    d1 = v[IX['D1mr']] + v[IX['D1mw']]
    ll = v[IX['DLmr']] + v[IX['DLmw']]
    return (v[0] * BASE_CPI + (d1 - ll) * D1_PENALTY + ll * LL_PENALTY +
            (v[IX['Bcm']] + v[IX['Bim']]) * MISPREDICT)


def main(path, ops):
    sf, sff, ed = parse(path)
    T = {e: sum(v[IX[e]] for v in sf.values()) for e in EV}
    ir = T['Ir']
    print(f"profile: {path}\nmeasured ops: {ops:,}\n")

    d1 = T['D1mr'] + T['D1mw']
    ll = T['DLmr'] + T['DLmw']
    refs = T['Dr'] + T['Dw']
    apply_ll = sum(v[IX['DLmr']] + v[IX['DLmw']] for fn, v in sf.items() if 'apply' in fn)
    print("== totals (MEASURED, engine region only; cache/branch numbers are simulated) ==")
    print(f"instructions       {ir:,}  = {ir / ops:.0f} per op   (loads {T['Dr'] / ops:.0f}/op, stores {T['Dw'] / ops:.0f}/op)")
    print(f"L1 data misses     {d1:,}  = {d1 / ops:.2f}/op  ({100 * d1 / refs:.2f}% of data refs)  [sim D1 48 KB 12-way]")
    print(f"LL data misses     {ll:,}  = {ll / ops:.3f}/op  ({100 * ll / refs:.3f}% of data refs)  [sim LL 24 MB 12-way]")
    print(f"  of which in apply() = harness reading the op array: {apply_ll:,} ({100 * apply_ll / max(ll, 1):.1f}%); "
          f"engine only: {(ll - apply_ll) / ops:.3f}/op")
    print(f"I1 / LL instr misses {T['I1mr']:,} / {T['ILmr']:,}")
    print(f"cond branches      {T['Bc'] / ops:.1f}/op, simulated mispredicts {T['Bcm'] / ops:.2f}/op ({100 * T['Bcm'] / T['Bc']:.1f}%)"
          f"  [Valgrind's predictor is crude; real hardware predicts better]")

    cats = collections.defaultdict(lambda: [0] * len(EV))
    for (fn, fl), v in sff.items():
        c = category(fn, fl)
        for i, x in enumerate(v):
            cats[c][i] += x

    print("\n== component table (MEASURED self cost, classified by function / inlined source file) ==")
    print(f"{'component':<52}{'Ir/op':>8}{'%Ir':>7}{'D1m/op':>8}{'misp/op':>9}")
    for c, v in sorted(cats.items(), key=lambda kv: -kv[1][0]):
        print(f"{c:<52}{v[0] / ops:>8.1f}{100 * v[0] / ir:>6.1f}%{(v[IX['D1mr']] + v[IX['D1mw']]) / ops:>8.3f}"
              f"{(v[IX['Bcm']] + v[IX['Bim']]) / ops:>9.3f}")

    # Allocator work by who called operator new / delete (MEASURED call counts and inclusive Ir).
    def callers(stub):
        rows = collections.defaultdict(lambda: [0, 0])
        for (c, d), e in ed.items():
            if d == stub:
                rows[c][0] += e[0]
                rows[c][1] += e[1][0]
        return rows
    new_rows, del_rows = callers('0x000000000010a480'), callers('0x000000000010a490')
    own = collections.defaultdict(float)   # owner -> Ir
    calls = collections.defaultdict(float)
    mixed_calls = mixed_ir = 0
    vec_new = 0
    for c, (n, x) in new_rows.items():
        if c.startswith('lob::OrderBook::rest'):
            own['resting order lifecycle'] += x; calls['resting order lifecycle'] += n
        elif 'match_against' in c:
            own['vector<Trade>'] += x; calls['vector<Trade>'] += n; vec_new += n
        elif '_Rb_tree' in c:
            own['std::map level nodes'] += x; calls['std::map level nodes'] += n
        else:
            own['unattributed'] += x
    vec_destroy = 0
    for c, (n, x) in del_rows.items():
        if '_Hashtable' in c:
            own['resting order lifecycle'] += x; calls['resting order lifecycle'] += n
        elif c == 'main':
            own['vector<Trade>'] += x; calls['vector<Trade>'] += n; vec_destroy += n
        elif 'match_against' in c:
            mixed_calls += n; mixed_ir += x
        elif 'cancel' in c:
            own['std::map level nodes'] += x; calls['std::map level nodes'] += n
        else:
            own['unattributed'] += x
    # ESTIMATE: match_against deletes = old vector buffers freed during growth + emptied map levels.
    vec_old = max(vec_new - vec_destroy, 0)
    share = min(vec_old / mixed_calls, 1.0) if mixed_calls else 0.5
    own['vector<Trade>'] += mixed_ir * share; calls['vector<Trade>'] += vec_old
    own['std::map level nodes'] += mixed_ir * (1 - share); calls['std::map level nodes'] += mixed_calls - vec_old
    alloc_total = sum(own.values())
    print("\n== allocator work by owner (calls MEASURED; the vector/map split of one delete bucket is an ESTIMATE) ==")
    for o, x in sorted(own.items(), key=lambda kv: -kv[1]):
        print(f"  {o:<28}{calls[o] / ops:>6.2f} calls/op{x / ops:>8.1f} Ir/op{100 * x / ir:>6.1f}%")
    print(f"  {'total':<28}{sum(calls.values()) / ops:>6.2f} calls/op{alloc_total / ops:>8.1f} Ir/op{100 * alloc_total / ir:>6.1f}%")

    # Components with allocator work attributed to the structure that owns the allocation.
    # ESTIMATE: resting-order lifecycle (2 allocations per order) split 50/50 Order vs hash node.
    life = own['resting order lifecycle']
    rows = [
        ('unordered_map (own code + node allocs)', cats['unordered_map (id index)'][0] + life / 2),
        ('vector<Trade> (own code + buffer allocs)', cats['vector<Trade> logic'][0] + own['vector<Trade>']),
        ('matching loop + book logic', cats['matching loop + book logic'][0]),
        ('std::map (own code + node allocs)', cats['std::map (price levels)'][0] + own['std::map level nodes']),
        ('Order objects (allocs only)', life / 2),
        ('glue/other (allocator wrappers, unique_ptr, memcpy, min)',
         cats['allocator/unique_ptr glue'][0] + cats['other (std::min/copy, memcpy, misc)'][0] + own['unattributed']),
        ('harness (not the engine)', cats['harness (apply wrapper, loop, checksum)'][0]),
    ]
    print("\n== components with allocator work attributed to the owning structure ==")
    print(f"{'component':<58}{'Ir/op':>8}{'%Ir':>7}{'%engine Ir':>12}")
    engine_ir = ir - cats['harness (apply wrapper, loop, checksum)'][0]
    for name, x in sorted(rows, key=lambda r: -r[1]):
        eng = '' if name.startswith('harness') else f"{100 * x / engine_ir:>11.1f}%"
        print(f"{name:<58}{x / ops:>8.1f}{100 * x / ir:>6.1f}%{eng:>12}")

    # Estimated time shares: cost model on engine categories only (the harness's op-array
    # streaming misses are excluded: hardware prefetch hides them, the simulator does not).
    eng_cyc = {c: cycles(v) for c, v in cats.items() if not c.startswith('harness')}
    alloc_c = eng_cyc['heap allocator (malloc/free/operator new/delete)']
    glue_c = eng_cyc['allocator/unique_ptr glue'] + eng_cyc['other (std::min/copy, memcpy, misc)']
    est = {
        'heap allocation, all owners': alloc_c,
        'matching loop + book logic': eng_cyc['matching loop + book logic'],
        'unordered_map ops (excl. allocation)': eng_cyc['unordered_map (id index)'],
        'std::map ops (excl. allocation)': eng_cyc['std::map (price levels)'],
        'vector<Trade> logic (excl. allocation)': eng_cyc['vector<Trade> logic'],
        'glue/other': glue_c,
    }
    tot = sum(est.values())
    print(f"\n== ESTIMATED share of engine time (model: {BASE_CPI} cycle/instr + {D1_PENALTY}/L1 miss + "
          f"{LL_PENALTY}/LL miss + {MISPREDICT}/mispredict; harness excluded; NOT hardware-validated) ==")
    for name, x in sorted(est.items(), key=lambda kv: -kv[1]):
        print(f"  {name:<42}{100 * x / tot:>6.1f}%")
    print(f"  model total {tot / ops:.0f} cycles/op")

    # Same estimate with the allocator's cycles handed to the owning structure in proportion to
    # its allocator instructions (ESTIMATE on top of an ESTIMATE).
    if alloc_total:
        life_f, vec_f, map_f = (own['resting order lifecycle'] / alloc_total,
                                own['vector<Trade>'] / alloc_total,
                                own['std::map level nodes'] / alloc_total)
        attributed = {
            'unordered_map (ops + its node allocs)': est['unordered_map ops (excl. allocation)'] + alloc_c * life_f / 2,
            'vector<Trade> (logic + buffer allocs)': est['vector<Trade> logic (excl. allocation)'] + alloc_c * vec_f,
            'std::map (ops + level node allocs)': est['std::map ops (excl. allocation)'] + alloc_c * map_f,
            'Order objects (allocs only)': alloc_c * life_f / 2,
            'matching loop + book logic': est['matching loop + book logic'],
            'glue/other': est['glue/other'],
        }
        print("\n== ESTIMATED share of engine time with allocation attributed to the owning structure ==")
        for name, x in sorted(attributed.items(), key=lambda kv: -kv[1]):
            print(f"  {name:<42}{100 * x / tot:>6.1f}%")

    # Hotspot tables (MEASURED).
    print("\n== top 10 functions by SELF cost (inlined callee code merged into the function) ==")
    for fn, v in sorted(sf.items(), key=lambda kv: -kv[1][0])[:10]:
        print(f"{v[0] / ops:>7.1f} Ir/op {100 * v[0] / ir:>5.1f}%  {short(fn)}")
    skip = ('main', '(below main)')
    inc = {}
    for fn, v in sf.items():
        inc[fn] = v[0] + sum(e[1][0] for (c, d), e in ed.items() if c == fn)
    print("\n== top 10 functions by INCLUSIVE cost (PLT stubs and program entry points omitted) ==")
    shown = 0
    for fn, x in sorted(inc.items(), key=lambda kv: -kv[1]):
        if fn in skip or fn.startswith('0x') or fn.startswith('__libc'):
            continue
        print(f"{x / ops:>7.1f} Ir/op {100 * x / ir:>5.1f}%  {short(fn)}")
        shown += 1
        if shown == 10:
            break


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2]))
