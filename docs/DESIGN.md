# Design decisions

One entry per decision. Each has: Chosen, Rejected alternative, Why, Tradeoff, Evidence.

Evidence points at tests, commits, or numbers in docs/RESULTS.md. If I did not measure something, the entry says so. All timings come from one machine (an Intel Core Ultra 9 185H under WSL2), so they are noisy. Entry 12 explains how I deal with that.

## 1. Integer ticks instead of doubles

**Chosen:** Prices are `int64_t` ticks, a whole number of ticks, never floating point.

**Rejected alternative:** Storing prices as `double`.

**Why:** Matching is "does this price cross that price", and that must never depend on rounding. A price like 100.10 has no exact binary form, so two orders that should share a level could end up on different ones. Integers also let me index an array by `price - base`, which the level array uses later.

**Tradeoff:** The caller has to convert to ticks. There is nothing smaller than one tick. The tick size is fixed outside the engine.

**Evidence:** `Order::price` is `int64_t` in `include/lob/types.hpp`. Unit tests check prices at the ends of the int64 range (`ExtremePricesAreHandledAroundTheWindow`, `ExtremePricesRestAndMatch`). I did not benchmark doubles against integers.

## 2. Single-threaded matching per symbol, no locks

**Chosen:** One thread owns one `OrderBook`. No mutexes and no atomics inside it.

**Rejected alternative:** A shared book with locks, or lock-free structures.

**Why:** Order N must see the book exactly as order N-1 left it. One owner keeps that rule simple and keeps results deterministic, so replay and fuzzing against the oracle work. It also keeps the linked lists easy to reason about.

**Tradeoff:** One symbol can use only one core. Scaling means one book per symbol or shard. There is no input queue yet, because the SPSC ring buffer is not built.

**Evidence:** The benchmark runs one book on one thread pinned to core 2 (RESULTS.md, Machine). The checksum `002f8ec71a31eeb9` is identical in every default run. I did not measure the cost of locks, so I make no claim about it.

## 3. Price-time priority with an intrusive doubly linked FIFO per level

**Chosen:** Each price level holds `head`, `tail` and a total quantity. Each `Order` has `prev` and `next` pointers. Matching takes the order at the head of the best level. A trade happens at the resting order's price. A partly filled resting order keeps its place.

**Rejected alternative:** A `std::vector`, `std::deque` or `std::list` of orders per level.

**Why:** Cancel has to remove an order from the middle of a level. With intrusive links that is O(1) and allocates nothing. Orders have stable addresses because they live in a pool (entry 9).

**Tradeoff:** The list pointers live inside `Order`, so `Order` is tied to this design. Walking a level follows pointers, which is not cache friendly. I did not benchmark the alternatives.

**Evidence:** Tests cover a partial fill keeping the front of the queue, a cancel in the middle keeping the rest of the queue, and cancelling the last order at a level removing the level. `OrderBook::check_invariants()` checks the links in both directions, the level totals, and that no empty level exists.

## 4. Modify as cancel plus resubmit

**Chosen:** `modify` cancels the order and submits a new limit order with the same id and a new sequence number, so it goes to the back of the queue. If the id is not resting, nothing changes. A new quantity of 0 cancels the order. A modify can trade immediately if the new price crosses.

**Rejected alternative:** Changing the order in place and keeping its queue position when that is safe.

**Why:** Keeping position after changing price or size would let a trader jump ahead of orders that arrived earlier, which breaks time priority. Cancel plus resubmit is one simple rule that is easy to state and easy to check against the oracle.

**Tradeoff:** A modify always loses its place, even a size reduction. It costs a cancel and a submit, which means more id index work. I did not make any claim about what real exchanges do.

**Evidence:** Tests `ModifySendsOrderToBackOfLevel`, `ModifyCanCrossAndTrade`, `ModifyToZeroQtyCancelsTheOrder`, `ModifyMovesAnOrderAcrossTheWindowEdgeAndBack`. The demo prints "(new time priority)" after a modify.

## 5. A sequence counter instead of timestamps

**Chosen:** Time priority uses `seq`, a counter that goes up every time an order comes to rest. No clock appears anywhere in the matching rules.

**Rejected alternative:** Timestamps from a clock.

**Why:** The same input must give the same trades every time, on any machine. A counter does that. It also makes the engine easy to test against the oracle.

**Tradeoff:** `seq` only says which order arrived first at this book. It is not a real arrival time. The engine records no real timestamps.

**Evidence:** The comment on `Order::seq` says it is never a clock. The checksum is identical across all default runs in RESULTS.md. The fuzz test is deterministic for a given seed.

## 6. Differential fuzzing against NaiveMatcher as the main correctness proof

**Chosen:** `NaiveMatcher` is a reference made to be obviously correct, not fast: one `std::vector<Order>`, scanned fully on every fill. A seeded random generator feeds the same operations to `NaiveMatcher` and `OrderBook`. After every operation the test compares the trades (including their order), the cancel result, and the best bid and ask. Every 1000 operations it also compares `total_qty_at`, checks `depth()` is well formed, and calls `check_invariants()`. On the first mismatch it prints the seed, the operation index, both results and the last 10 operations.

**Rejected alternative:** Only hand-written unit tests, or only invariant checks.

**Why:** Random sequences find interactions I would not write down, like cancelling an id that was already filled, or a modify that crosses. The oracle is short enough to read and trust.

**Tradeoff:** The test is only as good as the oracle and the generator. If I misunderstood a rule, both implementations share the mistake. The oracle is slow, so runs are short: the defaults are 40,000 ops per seed in Debug and 250,000 in Release, across 6 seeds.

**Evidence:**
- The generator once picked cancel and modify targets from every id ever issued, so most missed. I added a bias toward recent ids. The test now asserts the cancel and modify hit rates stay above 8%. Measured hit rates are 16.9% to 18.2% in every scenario (RESULTS.md, Optimization 3).
- Five scenarios run against the oracle: the default in-window case, a window edge crossing, everything through the fallback, a wide price range (1..5000), and far-outside prices (5% of prices uniform within 2^62 of zero).
- Bug injection: I put bugs in by hand and checked the fuzz test failed. In `OrderBook` I tried buying with `<` instead of `<=`, and moving a partly filled order to the back of its queue. Both were caught, then reverted. In `price_levels.hpp` I tried three bugs (window bound `<=` instead of `<`, a best-price merge that ignores the fallback, a cursor that does not advance). All three were caught (RESULTS.md, Optimization 3). None were committed.

## 7. Measuring: pre-generated ops, pinned core, checksum, percentiles

**Chosen:** `lob_latency` generates all operations before timing starts, runs warm-up operations and discards their timings, then times each measured operation on its own with `steady_clock` into a preallocated vector. It pins the thread to a core, folds every returned trade into a checksum, and reports p50, p90, p99, p99.9, max and throughput. It refuses to run in a Debug build.

**Rejected alternative:** An average time per iteration, or generating operations inside the timed loop.

**Why:** Generation must not be in the timing. Averages hide the tail. The checksum proves two runs did the same work and that a change did not alter behavior.

**Tradeoff:** The timer costs about 26 ns per measurement and I do not subtract it. Throughput includes the timer calls and the checksum update. WSL2 adds noise, and max latency shows multi-millisecond outliers that say nothing about the code.

**Evidence:** RESULTS.md, Method. The same code run three times back to back gave p50 of 137, 157 and 142 ns and 3.49M to 4.11M ops/s (RESULTS.md, Baseline runs). That spread is why I report medians and ranges.

## 8. Profiling with Valgrind, because perf is not available on WSL2

**Chosen:** Callgrind gives instruction counts and call counts for the engine only. Toggles (`LOB_CALLGRIND`) make the profile skip harness setup. Cachegrind gives whole-program numbers. `tools/callgrind_breakdown.py` turns the raw profile into a component table.

**Rejected alternative:** `perf`. It is not installed, and the WSL2 kernel would not have matching tools.

**Why:** Instruction counts and call counts are exact and repeatable. They showed where the work went and ranked the optimizations.

**Tradeoff:** Cache and branch numbers are simulated. The simulated L1 data cache is 48 KB 12-way and the last-level cache is 24 MB, with no L2. Valgrind's branch predictor is cruder than a real one. The time shares come from a simple cost model I never checked against hardware counters. Instruction count is not time.

**Evidence:** RESULTS.md, Profile of the baseline, lists the caveats. In the identity-hash experiment (entry 10) simulated L1 misses fell from 1.65 to 0.99 per op and wall-clock time did not change measurably, so the model weights L1 misses more than real hardware does. The profiles are committed in `docs/profiles/`.

## 9. Order pool and a caller-supplied trade buffer

**Chosen:** `OrderPool` hands out `Order` slots from 4096-slot chunks. Free slots form a list threaded through the unused `Order::next` field. Addresses never move. Under ASan, released slots are poisoned, so reading one is still reported. `submit_limit`, `submit_market` and `modify` write into a `std::vector<Trade>&` that is cleared and reused. The old functions that return a vector stay as thin wrappers.

**Rejected alternative:** One heap allocation per order, and returning a new `vector<Trade>` from every call.

**Why:** The baseline profile showed allocator work at 256.6 instructions per op, about 43% of everything collected, at 3.18 allocator calls per op.

**Tradeoff:** The pool never shrinks. Its size stays at the peak number of resting orders. Callers have to reuse their buffer or they pay for allocation again. The wrappers allocate.

**Evidence:** RESULTS.md, Optimization 1. Instructions per op went from 596 to 374 (200k ops) and 375 (5M ops). Allocator calls went from 3.18 to 1.27 at 5M. On the interleaved scale throughput went from 6.07M to 7.20M ops/s (+18.6%). In the same-session 5-run sets the tail improved more than the median (p99 -39.9%, p50 -14.6%).

## 10. A flat open-addressed id index

**Chosen:** `IdIndex` replaces `unordered_map`. It is one contiguous array of 16-byte `{key, Order*}` slots, aligned to 64 bytes, with power-of-two capacity, linear probing and load kept under 0.5. Deletion uses backward shift, so there are no tombstones. The hash is a multiply: `((id ^ (id >> 32)) * golden) >> shift`. The constructor sizes the table for the expected number of resting orders. Growing is a slow path, and a resize counter lets the benchmark prove it never ran.

**Rejected alternative:** `unordered_map` with `reserve` and pooled nodes, intrusive chaining through a new `Order` field, a `vector<Order*>` indexed by id, tombstones, and the identity hash (tried as an experiment).

**Why:** In the baseline profile the `unordered_map` plus its node allocations cost 137.4 instructions per op, about 24.9% of engine instructions, and about half of the simulated L1 misses. `std::hash<uint64_t>` is the identity on libstdc++, so a power-of-two mask would cluster ids that share low bits. A multiply spreads them. Tombstones would fill chains with dead entries because this workload inserts and erases about 0.4 times per op. A `vector` indexed by id would be fastest here because the generator's ids are dense, but it would be wrong for sparse ids and would flatter the benchmark.

**Tradeoff:** The table is 4 MB at 31% load for 81k live orders. The multiply hash scatters sequential ids, so about 0.9 simulated L1 misses per op remain. A moved-from `IdIndex` can only be destroyed or assigned.

**Evidence:** RESULTS.md, Optimization 2. Instructions per op went from 375 to 284. On the interleaved scale throughput went from 7.20M to 9.71M ops/s (+34.8%). Twenty unit tests cover the table, including backward shift at every erase position and a randomized comparison with `std::unordered_map`. The identity-hash experiment cut simulated L1 misses from 1.65 to 0.99 per op, but three interleaved pairs showed no wall-clock difference. I did not adopt it because it is only safe for dense sequential ids.

## 11. A flat level array with a bitmap cursor and a std::map fallback

**Chosen:** `PriceLevels` keeps an array of levels indexed by `price - base` over a window of W ticks (default base 0, 4096 ticks). One bit per level marks the non-empty ones, and a cursor holds the best index. When the best level empties, `countl_zero` or `countr_zero` over the bitmap words finds the next one. Prices outside the window go to a `std::map` per side. That map is a slow path: it allocates and costs O(log n). `best()` merges the window and the map by price. `out_of_window_orders()` counts orders that used the slow path.

**Rejected alternative:** A window that recenters itself, a ring indexed by `price mod W`, a sorted vector of levels, a hash map from price to level, a two-level bitmap.

**Why:** After the earlier steps the `std::map` levels plus their node allocations cost 90.7 instructions per op (35.5% of engine instructions). I chose the bitmap over scanning the array because a scan gets slower as the window gets sparser.

**Tradeoff:** The window is fixed and nothing recenters it. If prices drift outside it, the engine stays correct but slow, and a level far from the window allocates.

**Evidence:** RESULTS.md, Optimization 3. Instructions per op went from 284 to 216. Allocator calls went from 0.49 to 0.00. In 12 alternating pairs the new code was faster in all 12, with a median gain of +15.5% and a paired ratio from 1.05 to 1.34. Two other interleaved sets gave +20% and +8%, so the size of the gain is uncertain. On the interleaved scale throughput went from 9.71M to 10.51M ops/s (+8.2%).

The honest part: the default workload only uses 21 adjacent prices (90 to 110), which is the best case for a flat array. The 4096-tick window and a 21-tick window ran at the same speed, but that only shows empty ticks are free. On a price band of 1..2000, a 4096-tick window covering it gave 10.58M ops/s. A 21-tick window gave 7.73M, and no window gave 7.87M. So missing the window costs about 26% in throughput and 38% to 48% in p99, and every run still gave the same checksum. I did not measure windows far larger than 4096 ticks, and I did not measure a book whose best level empties with the next one thousands of ticks away.

## 12. Interleaved A/B as the standard way to compare

**Chosen:** To compare an old build and a new build I run them alternately, and I alternate which goes first. I report the median and the paired ratios. The old commit is built in its own directory with the same Release flags.

**Rejected alternative:** Running 5 times before and then 5 times after, and comparing the two sets.

**Why:** This machine switches between a slow state and a fast state. Two sets run minutes apart can differ by 2x for reasons that have nothing to do with the code.

**Tradeoff:** It takes more runs. The result is a ratio with a range, not one clean number. It is still WSL2, so native Linux would be better.

**Evidence:** RESULTS.md, Optimization 2 and 3. For the level array, the sequential sets looked like +58% (6.07M before and 9.60M after). The 12 interleaved pairs gave +15.5%. A batch of identity-hash runs reached 8 to 12M ops/s in 3 of 5 runs, double anything else seen, and a control run minutes later did not reproduce it. Because of this the earlier rows in the Log are not on one scale. I re-measured all four versions interleaved (RESULTS.md, Optimization 3) and the README uses that table.

## 13. A multi-symbol router with global order ids

**Chosen:** `MatchingEngine` owns one `OrderBook` per symbol, kept in a `std::deque` so books never move, and listed in the order their symbols were first seen. Two flat tables do the lookups: symbol to book index, and the id of every resting order to its symbol. Both use `FlatMap`, a small copy of the `IdIndex` design (power-of-two capacity, linear probing, load under 0.5, backward-shift deletion, multiply hash). Order ids are global. An id that is resting on any symbol is rejected as a duplicate, whichever symbol the new order names. `cancel(id)` and `modify(id, ...)` find the book through the id table. `cancel(symbol, id)` also exists and returns false for an unknown symbol, an unknown id, or an id resting on a different symbol. A limit order creates its symbol's book the first time the symbol appears. A market order on an unknown symbol creates nothing, because there is no liquidity to take. The id table is kept up to date from the trades each call returns: a maker that is no longer resting leaves it.

**Rejected alternative:** `std::unordered_map` for either lookup. Requiring the symbol in every cancel and modify. Ids that are only unique per symbol. Finding an id by asking every book.

**Why:** The replay format has `CANCEL,<id>` and `MODIFY,<id>,...` with no symbol, so the engine has to resolve an id by itself. That forces ids to be unique across symbols, otherwise `CANCEL,7` is ambiguous. A flat table means routing allocates nothing. `unordered_map` would allocate a node for every resting order, and I did not want per-order allocation in the router when `OrderBook` has none. Asking every book would cost one lookup per symbol per cancel, which is fine for 3 symbols and bad for thousands.

**Tradeoff:** Global ids are a real restriction. A feed that reuses the same id on two symbols at the same time cannot be replayed through this engine unless the ids are made unique first. The router pays one extra `is_resting` probe per trade to keep the id table honest. Trades carry no symbol, so the caller has to know it (it is an argument of `submit_limit`, and `symbol_of(id)` gives it for a modify). The router allocates in exactly three places: when a symbol is first seen (a new `OrderBook`, with its pool and index), when the symbol table outgrows its presized capacity, and when the id table does. The last two are rare slow paths, and `id_table_resizes()` counts the second.

**Evidence:** `tests/matching_engine_test.cpp` and `tests/flat_map_test.cpp`. Tests cover symbols never trading against each other, an id resting on one symbol being rejected on another, an id becoming free again after a cancel or a fill, `cancel` of an unknown symbol returning false, and a presized engine never growing its id table. A randomized test runs 3 symbols (7, 1000 and 4000000000) through the engine against one `NaiveMatcher` per symbol, comparing every call's trades and the best prices. It re-submits recent ids on random symbols so cross-symbol duplicates and id reuse really happen, and it fails if there were none. I checked it by injecting two router bugs by hand (filled makers staying in the id table, and `cancel(symbol, id)` ignoring the symbol). Both were caught, then reverted. I have not measured the router's overhead.

