# Results

Latency numbers for the order book, one row per change. Every row is measured with the same
harness, seed and operation count so rows are comparable.

## Machine

| | |
|---|---|
| CPU | Intel(R) Core(TM) Ultra 9 185H |
| Logical cores | 22 |
| Memory visible to the OS | about 7.9 GB |
| OS | Linux 5.15.167.4-microsoft-standard-WSL2 (**WSL2**) |
| Compiler | g++ 13.3.0 |
| Build | Release, `-O3 -DNDEBUG` |
| Thread | pinned to core 2 with `sched_setaffinity` |

## Method

- Harness: `bench/latency_bench.cpp` (executable `lob_latency`). It refuses to run in a Debug
  build.
- Fixed seed (`--seed 1`) and fixed operation counts: 200,000 warm-up ops (timings discarded)
  then 5,000,000 measured ops. The generator uses its default config (prices 90 to 110, qty 1 to
  20, 55% limit / 10% market / 25% cancel / 10% modify, cancel and modify targets biased to
  recent ids).
- All operations are generated into a vector before any timing starts.
- Each measured operation is timed on its own with `std::chrono::steady_clock` into a
  preallocated vector. Up to and including the baseline row, the trade vector an operation
  returned was built inside the timed region (the engine's allocation counted) and destroyed
  after it. From the pooling row onward the harness reuses one trade buffer for every op, as the
  engine's buffer API is meant to be used. From the flat id index row onward the harness also
  sizes the id index for the expected number of resting orders (`--resting`, default one per 50
  ops) and prints how many times the index resized inside the measured loop (0 in every run).
  From the flat price-level row onward it also takes `--base`, `--window` (the price-level window,
  default base 0 and 4096 ticks, deliberately not fitted to the generator), `--price-min` and
  `--price-max` (the generated price band, default 90..110), and prints how many orders rested
  outside the window (0 in every default run).
- Percentiles, not averages (nearest-rank). Raw values: the median cost of the timer itself
  (two back-to-back `now()` calls, about 26 ns here) is reported but **not subtracted**.
- Throughput is measured operations divided by the wall time of the whole measured loop, so it
  includes the timer calls and the checksum update.
- Checksum: an order-sensitive hash over every returned trade (and cancel result) of the measured
  ops. The same seed and ops must give the same checksum on every run and for every
  implementation. If it changes, behaviour changed and the row is not comparable.
- Each baseline is run 3 times; the table records the median run (by p50).
- Pinned to one core, machine otherwise idle (checked before measuring).
- **Comparisons use interleaved A/B.** This machine switches between a slow and a fast state
  (see Optimization 2 and 3), so two back-to-back sets of 5 runs can differ by 2x for reasons
  unrelated to the code. From the flat price-level row onward the headline numbers come from runs
  of the old and new binaries alternating, with the order alternated too. Sequential sets are still
  reported but flagged.
- **WSL2 caveat:** timings are noisy (hypervisor scheduling, timer virtualization). Max latency
  shows multi-millisecond outliers that say nothing about the code. Treat differences of a few
  percent as noise, and take final numbers from native Linux if possible.

Reproduce:

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release -j
    ./build-release/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1 --pin-core 2

## Log

| Date | Change | Commit | p50 | p99 | p99.9 | Orders/sec | Notes |
|---|---|---|---|---|---|---|---|
| 2026-11-04 | baseline: std::map + unique_ptr orders | e5f469b | 142 ns | 890 ns | 1,719 ns | 3.89M | Median of 3 runs. 2,811,186 trades, 81,137 resting orders at end, checksum 002f8ec71a31eeb9, timer overhead 26 ns |
| 2026-11-04 | pool Orders + reuse trade buffer | 0b81941 | 135 ns | 583 ns | 1,081 ns | 4.54M | Median of 5 runs per metric, not one run. Same-session before: p50 158, p99 970, p99.9 1,733, 3.59M ops/s. Before and after ranges do not overlap. Checksum unchanged (002f8ec71a31eeb9). See "Optimization 1" below |
| 2026-11-04 | flat open-addressed id index | 131ab28 | 92 ns | 413 ns | 722 ns | 5.97M | Median of 5 runs per metric, same-session before: p50 121, p99 532, p99.9 990, 4.92M ops/s. Interleaved A/B against the previous code (6 pairs, every pair faster): 4.67M to 6.10M ops/s, +30.6%. Index resizes in measured loop: 0. Checksum unchanged (002f8ec71a31eeb9). See "Optimization 2" below |
| 2026-11-04 | flat price-level array (window 4096) | (this commit) | 47 ns | 195 ns | 384 ns | 11.81M | Median of 12 alternating-order A/B pairs against the previous code (10.22M ops/s, p50 56): +15.5%, all 12 pairs faster, paired ratio 1.05 to 1.34. Other interleaved sets gave +8% and +20%. Zero allocator calls per op. A 21-tick window is the same speed; on a 1..2000 price band a window that misses the traffic is about 26% slower. Earlier rows are from separate sessions and machine states, see the re-measure table in Optimization 3. Checksum unchanged (002f8ec71a31eeb9) |

## Baseline runs (run-to-run variation)

Same binary, same seed, 3 runs back to back on a quiet machine. Checksum, trades and resting
orders were identical in all three; only timing varied.

| Run | p50 | p90 | p99 | p99.9 | max | Orders/sec |
|---|---|---|---|---|---|---|
| 1 | 137 ns | 372 ns | 795 ns | 1,520 ns | 5.17 ms | 4.11M |
| 2 | 157 ns | 455 ns | 1,029 ns | 2,003 ns | 9.21 ms | 3.49M |
| 3 (median, logged above) | 142 ns | 401 ns | 890 ns | 1,719 ns | 2.07 ms | 3.89M |

## Profile of the baseline

Profile of the code measured in the baseline row (commit `e5f469b`: `std::map` price levels,
`unordered_map<id, unique_ptr<Order>>`, `vector<Trade>` returned per operation). Nothing has
been optimized, so there are no speedup claims here, only where the work goes.

### How it was measured

- `perf` is not installed here and WSL2 would not have matching kernel tools, so the profile uses
  Valgrind 3.22 (Callgrind and Cachegrind). **Cache and branch numbers are simulated**, not
  hardware counters. The simulated caches are L1d 48 KB 12-way and LL 24 MB 12-way (no L2
  level). Valgrind's branch predictor is much cruder than a real one, so mispredict counts are an
  upper bound on the trend, not a measurement of this CPU.
- Build: `build-profile`, Release plus `-g -fno-omit-frame-pointer`, still `-O3 -DNDEBUG`.
  `bench/latency_bench.cpp` has `#ifdef LOB_CALLGRIND` collect toggles around each engine call
  (and the freeing of its returned trades), so the profile contains only the order book, not
  the harness setup (op generation, 1M timer samples, sorting). The normal build is unchanged.
- Runs: 200,000 measured ops (as suggested) and the full 5,000,000 ops / 200,000 warm-up / seed 1
  of the baseline. The 5M profile has the same checksum as the baseline run
  (`002f8ec71a31eeb9`), so it is the same workload. Warm-up ops run uncollected.
- Instructions per op are identical at both sizes (596). The 5M run is used below because it has
  the real working set (81,137 resting orders at the end).

Reproduce:

    cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer -DLOB_CALLGRIND"
    cmake --build build-profile --target lob_latency
    valgrind --tool=callgrind --collect-atstart=no --cache-sim=yes --branch-sim=yes \
        --callgrind-out-file=callgrind.out \
        ./build-profile/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1
    python3 tools/callgrind_breakdown.py callgrind.out 5000000

Raw reports are in `docs/profiles/` (`callgrind_5m*.txt`, `callgrind_200k*.txt`,
`cachegrind_*_summary.txt`, and the raw `callgrind_*.callgrind` profiles the script reads).

### Machine-level picture (5M ops, engine only)

| Measure | Per op | Note |
|---|---|---|
| Instructions | 596 | 165 loads, 111 stores. Same at 200k ops |
| L1 data misses | 2.03 | 0.73% of data refs (0.61% at 200k) |
| Last-level data misses | 0.028 | The simulator reports 0.653/op, but 95.6% of that is the harness streaming its 208 MB op array (see Caveats). The book itself fits the 24 MB LL |
| Simulated branch mispredicts | 6.1 | 8.4% of 73 conditional branches per op. Crude predictor, treat as a trend |

Cachegrind, whole program (so it includes harness setup, not just the engine): 5M run has
5.87 G instructions (about 1,175 per measured op), D1 miss rate 1.3% (1.8% of reads), LL data miss
rate 0.4%. 200k run: 334.7 M instructions (about 1,670 per measured op), D1 miss rate 1.2%. The
per-op figure is inflated by setup, which is why the Callgrind engine-only numbers above are the
ones to use.

### Top hotspots (self cost, MEASURED, 5M ops)

Inlined library code is merged into the function it was inlined into.

| Rank | Function | Ir/op | % of collected | What it is |
|---|---|---|---|---|
| 1 | `_int_free` | 84.8 | 14.2% | glibc free path |
| 2 | `malloc` | 70.0 | 11.7% | glibc allocation |
| 3 | `OrderBook::rest` | 58.0 | 9.7% | inlined hash insert, map insert, unique_ptr, list link |
| 4 | `match_against<map<less>>` | 47.3 | 7.9% | matching loop, ask side (inlined map, vector, hash erase) |
| 5 | `free` | 47.2 | 7.9% | glibc free wrapper |
| 6 | `match_against<map<greater>>` | 46.9 | 7.9% | matching loop, bid side |
| 7 | `apply` (harness) | 43.4 | 7.3% | harness wrapper: Result move, switch, reading the op |
| 8 | `OrderBook::submit_limit` | 40.1 | 6.7% | duplicate-id lookup, dispatch |
| 9 | `operator new` | 24.8 | 4.2% | allocation entry point |
| 10 | `main` (harness) | 17.7 | 3.0% | harness loop and checksum |

By inclusive cost: `apply` 91.8%, `submit_limit` 64.0%, `rest` 27.3%, `operator delete` 24.2%,
the two `match_against` instantiations 21.9% and 21.7%, `operator new` 17.8%,
`unordered_map::_M_erase` 14.9%. (`submit_limit` is large because 55% of ops are limit orders and
it includes both matching and resting.)

Allocator traffic is 3.18 calls per op, about 1.6 `new` and 1.6 `delete`. Frees nearly match
allocations; the small difference is orders still resting at the end.

### Where the time goes, by component

Measured columns come straight from the profile. The estimated columns are labeled; they use a
simple cost model (0.35 cycle per instruction, 14 per L1 miss, 250 per LL miss, 15 per simulated
mispredict) that has **not** been validated against hardware counters. Trust the ranking more
than the exact percentages.

| Component | Ir/op (MEASURED) | % of engine instructions | Est. share of engine time (ESTIMATE) |
|---|---|---|---|
| `unordered_map`: own code + its node allocations | 137.4 | 24.9% | about 29% |
| `vector<Trade>`: own code + buffer allocations | 127.1 | 23.0% | about 19% |
| Matching loop and book logic (`order_book.cpp/hpp`) | 99.0 | 17.9% | about 18% |
| `std::map`: own code + level-node allocations | 89.3 | 16.2% | about 18% |
| `Order` objects (allocations only) | 65.0 | 11.8% | about 9% |
| Glue and other (allocator wrappers, `unique_ptr`, memcpy, min) | 31.7 | 5.7% | about 6% |
| Harness (not the engine) | 44.5 | n/a | excluded |

The same data without handing allocations to their owners, so you can see them as one block:

| Component | % of engine instructions | Est. share of engine time |
|---|---|---|
| Heap allocation (`malloc`, `free`, `operator new/delete`), all owners | 46.5% (256.6 Ir/op) | about 36% |
| Matching loop and book logic | 17.9% | about 18% |
| `unordered_map` own code, excluding allocation | 13.1% (72.4 Ir/op) | about 20% |
| `std::map` own code, excluding allocation | 9.4% (51.9 Ir/op) | about 13% |
| `vector<Trade>` own code, excluding allocation | 6.9% (37.9 Ir/op) | about 6.5% |
| Glue and other | 5.7% | about 6% |

What is measured and what is estimated:

- Measured: every instruction count, call count, per-function and per-source-file cost, and the
  simulated cache misses and mispredicts.
- Measured by call site: allocator work split by caller (1.57 calls per op for resting orders,
  1.14 for `vector<Trade>` buffers, 0.48 for `std::map` level nodes).
- Estimated: the 50/50 split of resting-order allocator work between the `Order` object and the
  `unordered_map` node (two allocations per resting order, similar cost); the split of one mixed
  delete bucket between `vector<Trade>` buffers and map nodes (done by call counts); and all
  time shares.
- `unordered_map` is also the main source of L1 misses: 1.0 of the 2.03 per op (50%), against
  0.625 from the harness reading its op array, 0.28 from memcpy and other, and under 0.07 from
  the allocator and 0.04 from the matching loop.

### What to optimize first (ranked, with evidence)

Overlaps exist (the hash node allocation counts under both 1 and 2), so these shares do not
add up and none of them is a promised gain.

1. **Stop allocating on the hot path.** Evidence: allocator work is 256.6 Ir/op, 43% of
   everything collected and 46.5% of engine-only instructions, at 3.18 allocator calls per op;
   `_int_free`, `malloc` and `free` are the top three functions. Sources, all measured: 1.57
   calls per op for resting orders (the `Order` plus its hash node), 1.14 for `vector<Trade>`
   buffers (growth reallocations plus freeing the result), 0.48 for `std::map` level nodes.
   Direction: an object pool for `Order`, and a caller-supplied output buffer instead of a
   returned `vector<Trade>`. Strongest evidence of the five because it is measured twice (call
   counts and instruction share).
2. **Replace the `unordered_map` id index.** Evidence: 137 Ir/op including its node allocations
   (about 25% of engine instructions, about 29% of estimated time) and half of all L1 data
   misses (1.0 per op). The misses sit in the id lookups at the top of `submit_limit` (39% of all
   L1 read misses), `cancel` (8%), `submit_market` (7%) and `modify` (3%). Direction: a flat open-addressed table or a slot array with pooled orders, which also
   removes the node allocation counted in item 1.
3. **Fix the `vector<Trade>` result path.** Evidence: 127 Ir/op including buffer allocations
   (about 23% of engine instructions). Most of that is allocator work already counted in item 1,
   so doing item 1 captures most of it. Remaining own cost is 37.9 Ir/op of push_back and
   move logic.
4. **Rethink `std::map` price levels.** Evidence: 89 Ir/op including 0.48 node allocations per
   op (levels are created and destroyed constantly, since a level is erased the moment it
   empties), about 16% of engine instructions, and 1.4 simulated mispredicts per op in tree
   descent and rebalancing. Direction: a flat array of levels over the price band plus a
   best-price cursor. Lower priority than 1 and 2 because prices are only ever 90 to 110 in this
   workload; check that this does not just overfit the generator.
5. **Matching loop proper last.** Evidence: 99 Ir/op (about 18%), but it is the actual work and
   it contains the inlined calls to everything above. Revisit after 1 to 4.

### Caveats

- **Simulated, not hardware counters.** No `perf` here. Valgrind models L1 and last-level
  caches but not L2, so real L1 misses probably cost less than modeled (a P-core L2 absorbs many
  of them), and the simulator's branch predictor is weaker than the real one.
- **The harness inflates simulated LL misses.** 95.6% of the 0.653 LL misses per op come from
  `apply` reading the pre-generated op array (40 bytes per op, 208 MB, one new cache line per
  1.6 ops, no prefetching in the simulator). Real hardware prefetches this stream well. The
  engine's own LL misses are about 0.028 per op, so the 81,137-order book still fits the
  simulated 24 MB LL.
- **Harness code is in the collected region** (`apply` wrapper 43.4 Ir/op and `main` 17.7 Ir/op,
  about 10% of collected instructions). It is reported as a separate row, not as engine cost.
  Part of the `apply` cost is the price of returning a `vector<Trade>` by value.
- **Absolute cycles are not trustworthy.** The model gives about 312 cycles per op for the
  engine, but wall-clock measurement and an unknown, WSL2-reported clock make it impossible to
  validate here. Use the shares, not the cycle counts.
- Different book sizes change cache behavior but not instruction counts: 200k and 5M ops both
  give 596 Ir/op. D1 miss rate rose from 0.61% to 0.73% between them.

## Optimization 1: pool Orders, reuse the trade buffer

Change: `Order` objects come from an `OrderPool` (4096-slot chunks, free list threaded through
the unused `Order::next`, stable addresses, no per-order `new`/`delete` once the pool has grown
to the peak number of simultaneously resting orders). `submit_limit`, `submit_market` and
`modify` write into a caller-supplied `std::vector<Trade>` that is cleared and reused; the old
vector-returning functions remain as thin wrappers. The id index (`unordered_map`, now mapping to
`Order*`) and the `std::map` levels are untouched, as is `NaiveMatcher`. Under ASan the pool
poisons released slots, so a read of a released order is still reported (checked with a scratch
program).

Behavior is identical: the checksum is `002f8ec71a31eeb9` in all 10 runs below, the fuzz test
(6 seeds at default sizes, `OrderBook` driven through one long-lived reused buffer) passes in
Debug with ASan/UBSan and in Release, and trade and resting-order counts are unchanged.

### Latency and throughput (Release, pinned core 2, 5M ops, seed 1, 5 runs each, same session)

| Metric | Before: median (min to max) | After: median (min to max) | Change of median |
|---|---|---|---|
| p50 | 158 ns (146 to 166) | 135 ns (129 to 142) | -14.6% |
| p90 | 444 ns (416 to 465) | 292 ns (276 to 324) | -34.2% |
| p99 | 970 ns (910 to 999) | 583 ns (527 to 660) | -39.9% |
| p99.9 | 1,733 ns (1,703 to 6,954) | 1,081 ns (968 to 1,222) | -37.6% |
| Orders/sec | 3.59M (3.44M to 3.73M) | 4.54M (4.27M to 4.83M) | +26.4% |

For every metric the before range and the after range do not overlap, and each change is larger
than the spread within a run set (spread of throughput: about 8% before, 12% after). The one
before-run p99.9 of 6,954 ns is a WSL2 outlier; the median ignores it.

Compare only within a session. The baseline row was measured earlier the same day at 3.89M
ops/s with the same binary behavior; the "before" runs here, on the same code, gave a 3.59M
median, so machine state drifts by roughly 8% between sessions.

### Engine-only profile (Callgrind, same method as the baseline profile)

| | Before | After |
|---|---|---|
| Instructions per op | 596 | 374 (200k ops), 375 (5M ops) |
| Allocator calls per op | 3.35 (200k), 3.18 (5M) | 1.35 (200k), 1.27 (5M) |
| Allocator instructions per op | 257 | 104 |
| Simulated L1 data misses per op | 1.70 (200k), 2.03 (5M) | 1.51 (200k), 2.03 (5M) |

What still allocates (5M, per op): 0.78 calls for the id-index node (about 0.39 new and 0.39
delete, one pair per resting order) and 0.49 for `std::map` level nodes. Those are exactly the
two structures this change left alone. `vector<Trade>` and `Order` allocations are at zero.

Engine instruction share after the change (5M): `unordered_map` with its node allocations 37.7%,
matching loop 27.8%, `std::map` with its node allocations 26.0%, glue and other 6.1%,
`vector<Trade>` 1.8%. By the same cost model as the baseline profile (an estimate, not hardware
validated) the engine goes from about 312 to about 231 cycles per op, a 26% reduction, which
agrees with the measured throughput gain.

### Notes and surprises

- **Allocator calls fell to 1.3 per op, not the roughly 2 expected.** Only about 0.42 orders
  rest per op, so the id-index node costs about 0.39 new plus 0.39 delete per op. The earlier
  estimate counted one node allocation per op instead of per resting order.
- **Instructions fell 37% but throughput rose 26%.** Simulated L1 data misses per op did not
  move at 5M (2.03 before and after), and half of them are `unordered_map` lookups, which this
  change did not touch. Memory behavior of the id index is now a larger share of what is left.
- **The tail improved more than the median** (p99 -40%, p99.9 -38%, p50 -15%). Allocation cost
  is spiky (malloc slow paths, vector growth, free consolidation), so removing it helps the
  slow operations most.
- The pool never shrinks: its footprint stays at the peak number of simultaneously resting
  orders. That is the intended trade for no per-order heap traffic.
- The "before" runs use the baseline harness behavior (vector returned per op, allocated inside
  the timed region) and the "after" runs reuse one buffer, so the measured gain includes both
  halves of the change, as the task intended.
- Raw reports for the after-profile are in `docs/profiles/after_pool_*` (the `.callgrind` files
  are the raw profiles; regenerate the breakdowns with `tools/callgrind_breakdown.py FILE OPS
  --pooled`).

## Optimization 2: flat open-addressed id index

Change: `unordered_map<uint64_t, Order*>` is replaced by `IdIndex` (`include/lob/id_index.hpp`),
a flat open-addressed hash table. One contiguous 64-byte-aligned array of 16-byte
`{key, Order*}` slots, power-of-two capacity, linear probing, load factor strictly under 0.5,
backward-shift deletion (no tombstones). Hash: `((id ^ (id >> 32)) * golden) >> (64 - log2 cap)`,
one multiply, because `std::hash<uint64_t>` is the identity on libstdc++ and a power-of-two mask
would then cluster ids that share low bits (unordered_map hides that with a prime modulus, which
costs a division per lookup). The table is sized from an expected entry count passed to the
`OrderBook` constructor and never allocates while the book stays under that count; growing
(double and rehash) is a documented rare slow path with a `resize_count()` so a benchmark can
prove it did not run. The `std::map` levels and `NaiveMatcher` are untouched.

Rejected alternatives: `unordered_map` with `reserve` and pooled nodes (keeps bucket then node
chasing and the modulo); intrusive chaining through a new `Order` field (bigger `Order`, a miss
walks Orders); a `vector<Order*>` indexed directly by id (fastest here because the generator's ids
are dense, but wrong for sparse or client-chosen ids and would flatter the benchmark); tombstones
(this workload inserts and erases about 0.4 times per op, so chains would fill with dead entries
and force rehash spikes); Robin Hood, quadratic or double hashing, SIMD group probing (more
complexity or worse locality than plain linear probing at load under 0.5).

Behavior is identical: the checksum is `002f8ec71a31eeb9` in every run, the fuzz test passes at
default sizes in Debug with ASan/UBSan and in Release (trade counts unchanged), and 20 new tests
cover the table (collision chains, backward shift for every erase position including a chain that
wraps the end of the array, growth, no growth when presized, steady churn, strided keys, and a
randomized comparison with `std::unordered_map`) plus two `OrderBook` tests.

### Latency and throughput (Release, pinned core 2, 5M ops, seed 1, 5 runs each, same session)

| Metric | Before: median (min to max) | After: median (min to max) | Change of median |
|---|---|---|---|
| p50 | 121 ns (118 to 131) | 92 ns (87 to 100) | -24.0% |
| p90 | 266 ns (263 to 290) | 224 ns (186 to 234) | -15.8% |
| p99 | 532 ns (511 to 553) | 413 ns (368 to 428) | -22.4% |
| p99.9 | 990 ns (907 to 1,066) | 722 ns (640 to 779) | -27.1% |
| Orders/sec | 4.92M (4.77M to 5.21M) | 5.97M (5.75M to 6.35M) | +21.2% |

Ranges do not overlap for any metric, and the throughput spread within a set is about 9% before
and 10% after, so +21% is outside the noise. The id index was sized at 262,144 slots for 104,000
expected resting orders (81,137 actually rest at the end): 0 resizes in total, 0 in the measured
loop, in all 5 runs.

### Cross-check: interleaved A/B

The two sets above were taken minutes apart, and this machine's state moves (see the note below),
so the pre-change code (built from the previous commit with the Release flags and no profiling
hooks) was also run against the new code alternately, 6 pairs:

| Metric | Old: median (min to max) | New: median (min to max) | Change of median |
|---|---|---|---|
| p50 | 134 ns (121 to 138) | 94 ns (90 to 101) | -29.7% |
| p90 | 284 ns (256 to 293) | 206 ns (195 to 216) | -27.4% |
| p99 | 510 ns (469 to 550) | 356 ns (334 to 374) | -30.1% |
| p99.9 | 892 ns (846 to 1,231) | 592 ns (557 to 632) | -33.6% |
| Orders/sec | 4.67M (4.57M to 5.17M) | 6.10M (5.82M to 6.41M) | +30.6% |

The new code was faster in every one of the 6 pairs (throughput ratios 1.24 to 1.32). The
sequential figure (+21%) is the conservative one to quote; the interleaved figure is the more
reliable estimate of the gain.

### Engine-only profile (Callgrind) and whole-program Cachegrind

| | Before (previous commit) | After |
|---|---|---|
| Instructions per op | 374 (200k ops), 375 (5M ops) | 281 (200k), 284 (5M) |
| Allocator calls per op | 1.35 (200k), 1.27 (5M) | 0.52 (200k), 0.49 (5M) |
| Allocator instructions per op | 104 | 39 |
| Simulated L1 data misses per op | 1.51 (200k), 2.03 (5M) | 1.58 (200k), 1.65 (5M) |
| Id index own code, instructions per op (5M) | 66, plus about 65 for its node allocations | 31, no allocation |
| Id index L1 data misses per op (5M) | 0.98 | 0.91 |
| Simulated mispredicts per op (5M) | 5.30 | 4.49 |

Cachegrind, whole program (so it includes harness setup, which is why instructions per op are
higher than the engine-only figures): 5M run 920 to 819 instructions per measured op, D1 miss
rate 1.7% to 1.8%, last-level data miss rate 0.6% to 0.7%; 200k run 1,385 to 1,289 instructions
per op, D1 1.3% to 1.5%, LL 0.3% to 0.3%. The miss rates rise slightly because the denominator
(data references) fell more than the number of misses; misses per measured op fell from 6.15 to
5.43 at 5M and rose from 7.42 to 8.21 at 200k.

What still allocates (5M, per op): 0.49 calls for `std::map` level nodes, nothing else.

Engine instruction share after the change (5M): matching loop and book logic 37.5%, `std::map`
with its node allocations 35.5%, id index 12.0%, `vector<Trade>` 7.5%, glue and other 7.0%. By the
baseline cost model (an estimate, not hardware validated) the engine is about 181 cycles per op,
down from about 231: `std::map` operations plus level-node allocations about 36%, matching loop
about 31%, id index about 20%.

### Experiment (not committed): is the L1 miss count about the hash?

The expectation going in was about 1 L1 data miss per op, and the measurement was 1.65. The
id index still takes about 0.9 simulated misses per op because the multiply hash scatters
sequential ids across the 4 MB table, so each lookup lands on a different cache line, while the
generator mostly touches recent ids. To test that, the same table was run with an identity hash
(slot = id masked), which keeps sequential ids adjacent:

| | Mixed hash (committed) | Identity hash (experiment) |
|---|---|---|
| Simulated L1 data misses per op (5M) | 1.65 | 0.99 |
| Id index L1 misses per op | 0.91 | 0.27 |
| Instructions per op | 284 | 282 |
| Wall clock, 3 interleaved pairs | 5.60M, 5.84M, 6.18M ops/s | 5.95M, 5.70M, 6.44M ops/s |

The mechanism is confirmed, and it did not matter for speed: an L1 miss served by L2 costs on the
order of 14 cycles, about 0.6 fewer misses per op is under 10 cycles, and an op costs several
hundred cycles, so the wall-clock difference is below the noise. The identity hash is not adopted:
it is only safe for ids that happen to be dense and sequential (the strided-key test shows why
the mix exists), and it buys nothing measurable. Raw profile:
`docs/profiles/experiment_identity_hash_callgrind_5m.callgrind`.

### Notes and surprises

- **The machine has a fast mode.** An unrelated batch of 5 runs of the identity-hash binary gave
  8 to 12M ops/s (p50 about 48 ns) in 3 of 5 runs, double anything else seen, and the interleaved
  control a few minutes later did not reproduce it for either variant. So a 2x swing from machine
  state alone is possible here (frequency or scheduling on WSL2), and back-to-back sequential
  sets can be misleading. This task used interleaved A/B as a cross-check, and doing so for
  future optimizations is recommended.
- Instructions per op fell 24% (375 to 284); throughput rose 21% to 31%. The id index lost
  more than half of its own instructions (66 to 31 per op) and all of its allocation work.
- L1 misses per op fell at 5M (2.03 to 1.65) but not at 200k (1.51 to 1.58): with a small book the
  old node-based map kept its hot nodes close together, while the flat table spreads ids over a
  256 KB array. At realistic sizes the flat table wins.
- Memory trade: the table is 4 MB at 31% load for the 81k live orders. The 0.5 load limit is what
  keeps probe chains short, so some sparsity is the price.
- Simulated mispredicts per op fell from 5.30 to 4.49; the id index share fell from 1.36 to 0.69.
  Valgrind's predictor is crude, so treat this as a trend only.

### Next target

The profile now points at the `std::map` price levels. Together with their node allocations they
are 90.7 instructions per op (35.5% of engine instructions, about 36% of estimated time) and the
only remaining source of allocation (0.49 calls per op, because a level is created and destroyed
whenever it fills or empties). They also contribute 1.40 of the 4.49 simulated mispredicts per op
through tree descent and rebalancing. The matching loop itself is second (37.5% of instructions,
about 31% of estimated time) and contains the inlined map operations, so it should be revisited
after the levels. Candidate direction: a flat array of price levels indexed by tick offset, with
a best-price cursor, checking first that the price band assumption does not just overfit the
generator.

## Optimization 3: flat price-level array

Change: the `std::map` price levels are replaced by `PriceLevels` (`include/lob/price_levels.hpp`),
one per side. Prices in a window of W ticks starting at `base` index a contiguous array of
`PriceLevel {head, tail, totalQty}` (24 bytes, defined in `include/lob/price_level.hpp`). The
in-window test is a single unsigned compare, `uint64(price) - uint64(base) < W`, which is also right
at the ends of the int64 range. A bitmap with one bit per level and a cursor holding the best
non-empty index make "best price" an array read; when the best level empties, the next best is found
with `countl_zero` / `countr_zero` over bitmap words (at most W/64 word reads, typically one).
`base` and `W` are `OrderBook` constructor arguments (defaults 0 and 4096). The pool, `IdIndex` and
`NaiveMatcher` are untouched.

Prices outside the window still work, through a `std::map` per side used only for them. **That is a
slow path: it allocates a node per level and costs O(log n).** `best()` merges the two by comparing
the window's best price with the map's best price, so matching walks the levels of both in correct
price order; `depth()` merges the two sorted sequences. `OrderBook::out_of_window_orders()` counts
orders that rested out of window; while it is 0 the map has never been populated and none of the
fallback code has run. It was 0 in every measured region of every default run below.

Best-price scan versus bitmap: I chose the bitmap because scanning the level array for the next
non-empty level is O(W) every time the best level empties, which gets worse exactly as the window gets
sparser, the failure the wide-window runs are meant to expose. Rejected: a rebasing window (moves
every level, and still needs a fallback), a ring keyed by `price mod W` (aliases distant prices), a
sorted vector of levels (O(n) inserts), a hash map from price to level (flat, but no best-price
order), a two-level bitmap (only worth it for windows in the millions of ticks).

### Correctness evidence

- 103 tests pass in Debug with ASan/UBSan and in Release (73 before): 13 `PriceLevels` unit tests
  (including a randomized comparison with a `std::map` reference over windows of 0, 1, 63, 64, 65, 130
  and 300 ticks, windows not a multiple of 64, extreme and negative prices), 13 `OrderBook` window tests
  that run each scenario against `NaiveMatcher` in lockstep (sweeps crossing both window edges, best
  cursor after the best level empties, cancel of the last order in a level, orders exactly at
  `base - 1`, `base`, `base + W - 1` and `base + W`, out-of-window orders matching in-window ones,
  modify across the edge), and 4 new fuzz scenarios.
- Fuzz scenarios, each against `NaiveMatcher` with 6 seeds, comparing trades, cancel results, best
  prices, `total_qty_at` on probe prices and `depth()` ordering, with `check_invariants()` every 1000 ops:

  | Scenario | Setup | Out-of-window rests per op (guard) | Ops per seed, Debug / Release |
  |---|---|---|---|
  | in-window | band 90..110, window 0..4095 | 0 (must be exactly 0) | 40,000 / 250,000 |
  | window-edge | band 90..110, window 95..104 | 0.175 (> 0.08) | 10,000 / 62,500 |
  | all-fallback | same band, window width 0 | 0.383 (> 0.19) | 10,000 / 62,500 |
  | wide-range | band 1..5000, window 2000..2999 | 0.294 (> 0.15) | 20,000 / 125,000 |
  | far-outside | band 90..110 plus 5% of prices uniform in +-2^62 | 0.0141 (> 0.007) | 10,000 / 62,500 |

  The cancel and modify hit-rate guards (above 8%) pass in every scenario (16.9% to 18.2%). The
  window-edge and all-fallback scenarios run the same generated stream with different windows, and
  their trade counts are identical, as they must be.
  The generator gained an optional far-price mode (`far_price_pct`, off by default and drawing no
  random numbers when off, so every earlier stream and the baseline checksum are unchanged).
- Mutation check: three bugs injected into `price_levels.hpp` one at a time were all caught
  (window bound `<=` instead of `<`: ASan heap-buffer-overflow in `acquire`; best-price merge that
  ignores the fallback: 3 fuzz scenarios plus the boundary test fail; cursor not advanced when the best
  level empties: fuzz mismatches, then a hang). The hang prompted a 600 s per-test timeout in CMake.
- The checksum is `002f8ec71a31eeb9` in every default 5M-op run, and the bench produces the same
  checksum for any `--base` / `--window` on the same generated stream.

### Latency and throughput

Headline, 12 alternating-order A/B pairs on the default 5M-op workload, pinned core 2 (old = previous
commit built with the Release flags, new = this commit with the default 4096-tick window):

| Metric | Old: median (min to max) | New: median (min to max) | Change of median |
|---|---|---|---|
| p50 | 56 ns (55 to 59) | 47 ns (46 to 49) | -16.1% |
| p90 | 127 ns (120 to 159) | 90 ns (84 to 118) | -29.5% |
| p99 | 244 ns (217 to 290) | 195 ns (174 to 289) | -19.9% |
| p99.9 | 437 ns (397 to 488) | 384 ns (318 to 518) | -12.2% |
| Orders/sec | 10.22M (9.09M to 10.52M) | 11.81M (10.58M to 12.80M) | +15.5% |

The new code was faster in all 12 pairs; the paired throughput ratio has median 1.16 and range 1.05 to
1.34. That range is the honest uncertainty: the gain is real, and its size is somewhere around 5% to
30%, best estimate about 15%. Two other interleaved sets gave +20.0% (6 rounds, 3 variants) and +8.2%
(5 rounds, 4 variants, one pair at 0.78 when the machine dropped into its slow state), so a quote of
"about 15%" is not conservative or inflated, but anything finer would be.

Window size, 6 interleaved rounds with the old code for reference: old 8.86M ops/s, new with a 4096-tick
window 10.62M (+20.0%), new with a window that just fits (`--base 90 --window 21`) 10.40M (+17.4%). Per
round the tight window was 0.87 to 1.04 times the wide one, median 1.00. On this workload a sparse
window costs nothing measurable. The simulated profile is identical for the two windows (216
instructions and 1.63 L1 misses per op), because the 21 occupied levels sit in the same bitmap word
and the same 504 bytes of the array either way.

The sequential 5-run sets taken at the start of the task (before, p50 92 ns and 6.07M ops/s; after,
p50 52 ns and 9.60M ops/s, range 6.40M to 10.31M) look like a +58% gain. **That figure is not
trustworthy**: the machine was in its slow state for the "before" set and mostly in its fast state for
the "after" set, and the first "after" run (6.40M) shows the transition. It is not used anywhere above.

Re-measure of the whole history on one scale (5 interleaved rounds, four builds, medians), because the
earlier Log rows come from separate sessions in different machine states and cannot be compared with
this row:

| Version | p50 | p99 | p99.9 | Orders/sec | Step |
|---|---|---|---|---|---|
| Baseline (`std::map`, `unordered_map`, heap Orders) | 91 ns | 561 ns | 1,041 ns | 6.07M | |
| Pooled Orders, reused trade buffer | 82 ns | 385 ns | 684 ns | 7.20M | +18.6% |
| Flat id index | 57 ns | 257 ns | 454 ns | 9.71M | +34.8% |
| Flat price levels | 48 ns | 220 ns | 414 ns | 10.51M | +8.2% |

Overall about 1.7x on the baseline workload. The last step reads +8% here and +15.5% in the 12-pair run.

### Engine-only profile (Callgrind, same method as before)

| | Before (flat id index, `std::map` levels) | After (flat levels) |
|---|---|---|
| Instructions per op | 281 (200k ops), 284 (5M ops) | 213 (200k), 216 (5M) |
| Allocator calls per op | 0.52 (200k), 0.49 (5M) | 0.00, 0.00 |
| Allocator instructions per op | 39 | 0 |
| Simulated L1 data misses per op | 1.58 (200k), 1.65 (5M) | 1.56 (200k), 1.63 (5M) |
| Simulated mispredicts per op | 4.27 (200k), 4.49 (5M) | 2.73, 2.96 |
| Window of 21 ticks, 5M | | 216, 0.00, 1.63, 2.96 (identical) |

The engine now performs no heap allocation at all in the measured region. Price-level cost per op
(5M): `std::map` 51.9 own instructions plus 38.8 for its node allocations (90.7) became 20.5 for the
array and bitmap plus 1.4 for `empty()` checks on the unused fallback map (21.9). The matching loop and
book logic stayed at 96 (95 after), the id index at 31, the trade-vector logic at 19 to 25 (some
classification drift between categories of a few instructions). By the baseline cost model (an
estimate, not hardware validated) the engine drops from about 181 to about 124 cycles per op.

L1 misses did not move: they come from the id index (0.91 per op) and, outside the engine, from the
harness reading its op array (0.63 per op). Raw profiles are in `docs/profiles/after_levels_*`.

### Overfitting risk and what the wide-window runs show

The generator draws prices from 90 to 110, 21 adjacent ticks, so almost everything that makes a flat
array look good is built into the workload: the occupied levels share a few cache lines, the best
cursor never moves more than a few ticks, and the bitmap scan never leaves one 64-bit word. The
4096-tick window being as fast as the 21-tick window therefore says only that an empty tick costs
nothing, not that a sparse book is cheap. To test the parts the default workload cannot, `lob_latency`
now takes `--price-min` and `--price-max`; here are three interleaved rounds on a band of 1..2000
(levels spread over 2000 ticks; a window that misses it sees about 39 out-of-window rests per 100 ops):

| Window | Orders/sec (median of 3) | p50 | p99 | Out-of-window rests in the measured loop |
|---|---|---|---|---|
| 4096 ticks at base 0 (covers the band) | 10.58M | 47 ns | 240 ns | 0 |
| 21 ticks at base 90 (covers about 1% of it) | 7.73M | 71 ns | 356 ns | 1,931,854 |
| width 0 (everything through the fallback) | 7.87M | 71 ns | 332 ns | 1,946,698 |

- With a window that covers a spread-out band, speed holds (10.6M, against 11.8M on the narrow band),
  so scanning a sparse bitmap is cheap at W=4096, where a scan is at most 64 words.
- When the window misses the traffic the engine falls back to the old structure's behavior: about 26%
  slower and a 38% to 48% worse p99, plus a heap allocation per new level. All three produce the same checksum
  (`9e169e3f9cf3a289` for this band), so the fallback is correct, only slow.
- So the benefit depends on the window being chosen to cover where prices trade. A fixed window is a
  real limitation: prices drift, and nothing here recenters the window. A production version would need
  to recenter or at least alarm on `out_of_window_orders()`. I did not measure windows far larger than
  4096 (for example a million ticks, where a bitmap scan can reach thousands of words and a two-level
  bitmap would be needed), nor a book whose best level empties with the next one thousands of ticks
  away, which is the worst case for the scan.

### Next target

With allocation gone, the profile is flat: no single structure is left to remove. Engine instructions
per op (5M, 188 of 216 excluding the harness): matching loop and book logic 95 (51%), id index 31
(16%), trade-vector logic 25 (13%), price levels 21 (11%), other 16 (8%). The model puts the matching
loop at about 43% of estimated time and the id index at about 29%, the latter because of its 0.9 L1
misses per op, although the identity-hash experiment showed those misses barely move wall-clock time.
The most concrete lead is redundant probing: `submit_limit` probes the id index once to reject a
duplicate and again to insert, `cancel` finds and then erases, and each fill erases by id again. Fusing
those into single-probe operations would trim both the id-index instructions and the book logic. After
that, line-level profiling of `match_against` and `rest` (which are 87 and 42 instructions per op) is
the way to find what the remaining 95 matching-loop instructions are spent on.
