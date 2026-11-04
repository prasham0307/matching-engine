# matching-engine

A limit order book matching engine in C++20 (one order book per symbol, routed by a small `MatchingEngine`), built to learn production-style C++, performance measurement and market microstructure. Prices are integer ticks. The engine matches limit, market, cancel and modify orders with price-time priority, and it is checked against a deliberately simple reference matcher with differential fuzzing. It was then optimized in measured steps. On the benchmark workload it went from 6.07M to 10.51M orders per second (interleaved measurements on one machine under WSL2, see Results for the caveats).

## Features

- Limit orders. A limit order matches while it crosses the best opposite price, and the rest of its quantity rests in the book.
- Market orders. They match at any price, and any unfilled quantity is dropped.
- Cancel, by order id.
- Modify, as a cancel plus a resubmit with the same id. The order loses its place in the queue. A new quantity of 0 cancels it.
- Price-time priority. The best price matches first, and orders at the same price match in arrival order. A trade happens at the resting order's price.
- Rejections: a quantity of 0, or an id that is already resting.
- `depth()`, `best_bid()`, `best_ask()` and `total_qty_at()` for reading the book.
- Multi-symbol routing with `MatchingEngine`: one book per symbol, order ids unique across symbols, cancel and modify by id alone.
- A reference matcher (`NaiveMatcher`), an interactive demo, a latency benchmark, and Valgrind profiling tools.

## Layout

    include/lob/
      types.hpp            Order, Trade, Side
      naive_matcher.hpp    the simple reference matcher used as the oracle
      order_book.hpp       one order book (one symbol)
      matching_engine.hpp  routes orders to one book per symbol
      flat_map.hpp         flat hash table used by the router
      order_pool.hpp       slab allocator for Orders
      id_index.hpp         flat open-addressed hash table, order id to Order*
      price_levels.hpp     flat level array with a bitmap and a std::map fallback
      price_level.hpp      one price level, an intrusive FIFO
      order_generator.hpp  seeded random operation stream
    src/                   naive_matcher.cpp, order_book.cpp, matching_engine.cpp
    tests/                 unit tests, window tests, and the differential fuzz test
    bench/                 latency_bench.cpp (lob_latency), smoke_bench.cpp (lob_bench)
    tools/                 demo.cpp (lob_demo), callgrind_breakdown.py
    docs/                  DESIGN.md, LEARNING.md, RESULTS.md, profiles/

## Requirements

- g++ 13 or another C++20 compiler
- CMake 3.20 or newer
- Internet access on the first configure (GoogleTest and Google Benchmark are fetched)
- Valgrind, only if you want to reproduce the profiles

## Build and test

Debug build, with AddressSanitizer and UndefinedBehaviorSanitizer:

    cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug -j
    ctest --test-dir build-debug --output-on-failure

Release build (`-O3`, also the default build type):

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure

There are 129 tests. The fuzz test runs 6 seeds. The default is 40,000 operations per seed in Debug and 250,000 in Release for the main scenario, and `FUZZ_OPS` overrides it.

## Run the demo

    ./build-release/tools/lob_demo            # runs on OrderBook
    ./build-release/tools/lob_demo --naive    # same commands on NaiveMatcher

Type `help` for the commands. A good first session:

    scenario quiz
    limit buy 102 8

You can also pipe commands in: `./build-release/tools/lob_demo < commands.txt`.

## Run the latency benchmark

    ./build-release/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1 --pin-core 2

It prints percentiles, throughput, a checksum, and the machine details. With the command above (5,000,000 operations, seed 1) the checksum must be `002f8ec71a31eeb9`. It refuses to run in a Debug build. Other options include `--base` and `--window` for the price-level window, and `--price-min` and `--price-max` for the generated price band. `bench/lob_bench` is only a smoke benchmark that checks Google Benchmark works.

## Reproduce the numbers

The first latency command and the profile commands below are copied from docs/RESULTS.md. The two extra `lob_latency` lines combine the options that docs/RESULTS.md describes (`--base 90 --window 21` for a window that just fits, and `--price-min 1 --price-max 2000` for the wider band).

Latency and throughput:

    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release -j
    ./build-release/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1 --pin-core 2

A window that just fits the default generator, and a wider price band:

    ./build-release/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1 --pin-core 2 --base 90 --window 21
    ./build-release/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1 --pin-core 2 --price-min 1 --price-max 2000

Engine-only profile with Valgrind (perf is not available on this machine):

    cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer -DLOB_CALLGRIND"
    cmake --build build-profile --target lob_latency
    valgrind --tool=callgrind --collect-atstart=no --cache-sim=yes --branch-sim=yes \
        --callgrind-out-file=callgrind.out \
        ./build-profile/bench/lob_latency --ops 5000000 --warmup 200000 --seed 1
    python3 tools/callgrind_breakdown.py callgrind.out 5000000

To compare two versions, I build the older commit in a separate directory and run the two binaries alternately, alternating which one goes first. docs/RESULTS.md explains why (see Results below).

## Results

All four versions below were measured together, interleaved, in 5 rounds, with the median of each metric. Workload: 5,000,000 operations, seed 1, one pinned core, prices 90 to 110.

| Version | p50 | p99 | p99.9 | Orders/sec | Step |
|---|---|---|---|---|---|
| Baseline (`std::map`, `unordered_map`, heap Orders) | 91 ns | 561 ns | 1,041 ns | 6.07M | |
| Pooled Orders, reused trade buffer | 82 ns | 385 ns | 684 ns | 7.20M | +18.6% |
| Flat id index | 57 ns | 257 ns | 454 ns | 9.71M | +34.8% |
| Flat price levels | 48 ns | 220 ns | 414 ns | 10.51M | +8.2% |

That is about 1.7x overall. Each step was a separate commit, and the checksum was identical for every one.

Caveats:

- These were measured on one machine, an Intel Core Ultra 9 185H under WSL2. WSL2 timings are noisy, and treat differences of a few percent as noise.
- The machine switches between a slow state and a fast state. Two separate sets of runs can differ by 2x for reasons unrelated to the code, which is why I compare interleaved.
- The last step is uncertain. It reads +8.2% in this table and +15.5% in a separate run of 12 alternating pairs (all 12 faster, paired ratio from 1.05 to 1.34).
- The workload is synthetic, with prices drawn from 21 adjacent ticks. That is a good case for a flat array. See Limitations.

The full tables, the method, the profiles, and the experiments that did not pan out are in [docs/RESULTS.md](docs/RESULTS.md).

## How correctness is checked

- `NaiveMatcher` keeps one vector of resting orders and scans all of it on every fill. It is short enough to read and trust, and it is the oracle.
- The differential fuzz test feeds the same seeded random operations to `NaiveMatcher` and `OrderBook`. After every operation it compares the trades (including their order), the cancel result, and the best bid and ask. Every 1000 operations it compares the quantity at each probed price, checks that `depth()` is well formed, and runs `check_invariants()`.
- Five scenarios: the default in-window case, a window edge crossing, everything through the fallback, a wide price range (1..5000), and far-outside prices. Guards fail the test if cancel or modify rarely hit a resting order (below 8%), or if a fallback scenario does not really use the fallback.
- Bugs injected by hand into the matching code and the level array were all caught by the fuzz or unit tests, then reverted.
- The Debug build runs everything under AddressSanitizer and UndefinedBehaviorSanitizer. The order pool poisons freed slots under ASan, so a read of a released order is reported.

More detail on each choice is in [docs/DESIGN.md](docs/DESIGN.md), and my mistakes and what I changed because of them are in [docs/LEARNING.md](docs/LEARNING.md).

## Limitations

- **Fixed price window.** Price levels live in a flat array over a window of ticks (default base 0, 4096 ticks). Prices outside it still work, through a `std::map`, but that is a slow path that allocates. On a price band of 1..2000, a window that missed the traffic was about 26% slower in throughput and 38% to 48% worse in p99. Nothing recenters the window. I did not measure windows far larger than 4096 ticks.
- **One thread.** Each order book matches one symbol, and the `MatchingEngine` router runs all symbols on one thread. There is no multi-threading and no input queue (the SPSC ring buffer is not built). Order ids must be unique across symbols. I have not benchmarked the router, only a single `OrderBook`.
- **No replay yet.** The benchmark uses a synthetic random order stream. Real market data replay is not built.
- **Simulated cache and branch numbers.** Without `perf`, the profile uses Valgrind. Its cache sizes are a model (no L2) and its branch predictor is crude. The time shares come from a cost model I did not check against hardware counters.
- **WSL2 noise.** All timings are from WSL2. Native Linux would be better for final numbers.
- **Synthetic workload.** The default workload draws prices from 90 to 110 and may flatter the flat price-level array.
