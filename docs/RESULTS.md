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
