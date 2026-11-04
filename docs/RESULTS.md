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
  preallocated vector. The trade vector an operation returns is built inside the timed region
  (the engine's allocation counts) and destroyed after it.
- Percentiles, not averages (nearest-rank). Raw values: the median cost of the timer itself
  (two back-to-back `now()` calls, about 26 ns here) is reported but **not subtracted**.
- Throughput is measured operations divided by the wall time of the whole measured loop, so it
  includes the timer calls and the checksum update.
- Checksum: an order-sensitive hash over every returned trade (and cancel result) of the measured
  ops. The same seed and ops must give the same checksum on every run and for every
  implementation. If it changes, behaviour changed and the row is not comparable.
- Each baseline is run 3 times; the table records the median run (by p50).
- Pinned to one core, machine otherwise idle (checked before measuring).
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
`cachegrind_*_summary.txt`, and the raw `callgrind_*.out` files the script reads).

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
