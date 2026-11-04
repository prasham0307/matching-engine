# Learning log

This is where I write down what I got wrong and what I will do differently. Each entry points at something real: a commit, a test, a number in docs/RESULTS.md, or a prediction I made before a measurement.

Each entry has four parts: Context, What I got wrong, Why, How I will spot it next time.

Some mistakes were made by Claude Code, not by me. I log those too and say who made them.

## Log

### 1. I guessed 1M to 3M ops/s for the baseline. It was 3.89M.

**Context:** Before the first benchmark run I predicted a p50 of 100 to 500 ns and a throughput of 1M to 3M ops/s.

**What I got wrong:** The p50 was 142 ns, inside my range. The throughput was 3.89M ops/s (RESULTS.md, baseline), above my range.

**Why:** I put the latency and the throughput guesses on separate scales. Throughput here is one over the mean latency, so it is pulled by the mean, and the mean was lower than I assumed from the median alone. I guessed the two numbers without checking they agreed with each other.

**How I will spot it next time:** When I predict two numbers that are linked, I will compute one from the other first. A p50 of 142 ns already implies several million ops/s.

### 2. I expected the id index to be about 1 L1 miss per op. It was 1.65.

**Context:** I replaced `unordered_map` with a flat open-addressed table (Optimization 2). I predicted about 1 simulated L1 data miss per op afterwards.

**What I got wrong:** The measurement was 1.65 per op, down from 2.03 (RESULTS.md, Optimization 2). The id index alone still took about 0.9 misses per op.

**Why:** My multiply hash scatters sequential ids across a 4 MB table, so each lookup lands on a different cache line. When I tried an identity hash, which keeps sequential ids next to each other, the misses did fall to 0.99 per op. But three interleaved pairs showed no wall-clock difference. An L1 miss that hits L2 is cheap, and an op costs several hundred cycles.

**How I will spot it next time:** Before I predict a miss count, I will ask where the keys come from and how a good hash spreads them. I will not treat a simulated miss count as time. I need a wall-clock check before I call it a win.

### 3. I expected about 2 allocator calls per op after the pool. It was 1.27.

**Context:** After pooling `Order` objects and reusing the trade buffer, the estimate was about 2 allocator calls per op (Optimization 1). The profile before the change said 3.18. This estimate was shared between me and Claude Code, and RESULTS.md records it as "roughly 2".

**What I got wrong:** The measured value was 1.27 at 5M ops and 1.35 at 200k (RESULTS.md, Optimization 1).

**Why:** Only about 0.42 orders rest per op. The id-index node is allocated once per resting order, not once per op. The estimate counted one node per op.

**How I will spot it next time:** I will write down the rate of the thing that allocates (resting orders per op) before I multiply by a cost. Counts per op are rates times sizes, so I derive the rate first.

### 4. I expected the tail to improve less than the median. It improved more.

**Context:** After the pool and buffer change I predicted that p50 would improve by 10% to 30% and that p99.9 would improve by less than p50. The measured p50 was 135 ns, just above my range of 100 to 130 ns (the same-session baseline was 158 ns, higher than the first baseline). RESULTS.md reports p50 down 14.6%, p99 down 39.9% and p99.9 down 37.6% (Optimization 1, same-session 5-run sets).

**What I got wrong:** I expected the tail to gain less than the median. It gained more.

**Why:** RESULTS.md's explanation is that allocation cost is spiky: malloc slow paths, vector growth and free consolidation. Removing it helps the slow operations most. That is a reasoning, and I did not test it separately. I had assumed the tail was mostly OS noise.

**How I will spot it next time:** I will ask which operations pay the cost I am removing, and look at the percentile where those operations live. A cost that only some operations pay shows up in the tail first.

### 5. I expected about 0.2 allocator calls per op after the level array. It was 0.00.

**Context:** Before Optimization 3 I predicted that allocator calls per op, which were 0.49, would drop to about 0.2.

**What I got wrong:** The measured value was 0.00 (RESULTS.md, Optimization 3).

**Why:** All 0.49 remaining calls came from `std::map` level nodes. Once levels live in a preallocated array, there is nothing left that allocates in the default workload. I guessed a partial drop without checking where the other calls would come from.

**How I will spot it next time:** When I predict how much of a cost a change removes, I will first list every source of that cost. If one source is the only one, the answer is all or nothing.

### 6. A +58% result that was really about +15%

**Context:** For the level array (Optimization 3), the first comparison was two separate sets: the old code at 6.07M ops/s and the new code at 9.60M ops/s.

**What I got wrong:** That looks like +58%. It is not trustworthy. The 12 alternating pairs gave +15.5%, with a paired ratio from 1.05 to 1.34 (RESULTS.md).

**Why:** This WSL2 machine switches between a slow state and a fast state. The old set ran in the slow state and the new set ran mostly in the fast state. Its first run (6.40M ops/s) shows the switch. A batch of identity-hash runs reached 8 to 12M ops/s in 3 of 5 runs, double anything else seen, and a control a few minutes later did not reproduce it. The same problem means the earlier Log rows in RESULTS.md come from separate sessions and are not on one scale. I re-measured all four versions interleaved to fix that.

**How I will spot it next time:** Any big win from two separate sets is suspect. I will look at the spread inside one set (the "after" set ranged from 6.40M to 10.31M ops/s), and I will alternate old and new runs before I believe a number.

### 7. The raw profiles were never committed (Claude Code's mistake)

**Context:** The commit "Profile the baseline" (c2077da) said the raw profiles were in `docs/profiles/`, and `docs/RESULTS.md` told people to use them.

**What I got wrong:** Claude Code wrote that claim without checking what git tracked. The raw files were named `*.out`, and `.gitignore` has a `*.out` rule (line 38). The commit contains only the text reports. It was caught in the next task, and the raw files were renamed to `.callgrind` and committed in 0b81941.

**Why:** `git add -A` skips ignored files without saying so. Nobody checked `git status` against the claim.

**How I will spot it next time:** When a commit claims a file is in the repo, I run `git ls-files` for it, or `git check-ignore -v`, before I write the claim.

### 8. The first fuzz generator mostly cancelled dead orders (Claude Code's mistake)

**Context:** The first generator (commit ea6a422) picked cancel and modify targets uniformly from every id ever issued, plus a few ids never issued.

**What I got wrong:** Almost all ids had long since been filled or cancelled, so most cancels and modifies missed. The fuzz test was exercising cancel and modify much less than it looked. The commit "Improve fuzz generator cancel coverage and scale up runs" (e8ff84b) added a bias toward recent ids.

**Why:** The set of "all ids ever issued" keeps growing, and the set of resting orders stays small. Picking uniformly from the first gives mostly dead targets.

**How I will spot it next time:** A test that generates input needs a number that shows the input is useful. The fuzz test now measures cancel and modify hit rates and fails if either drops below 8%. The measured rates are 16.9% to 18.2% (RESULTS.md, Optimization 3).

### 9. Two windows that run the same do not prove a sparse book is cheap

**Context:** With the level array, a 4096-tick window and a 21-tick window ran at the same speed (median paired ratio 1.00).

**What I almost got wrong:** I could have read that as "a big sparse window costs nothing".

**Why:** The generator only produces prices from 90 to 110, which is 21 adjacent ticks. They sit in one 64-bit word of the bitmap and in a few cache lines of the array. The workload hides the cost of sparsity. A run on a 1..2000 price band showed what a bad window really costs: about 26% slower and a 38% to 48% worse p99 when the window misses the traffic (RESULTS.md, Optimization 3).

**How I will spot it next time:** When a benchmark says "no difference", I will ask what the workload would have to look like to make a difference, and run that.

## Concepts I can explain without notes

A tick means I can explain the concept out loud without opening any file. I only tick after I have tried. The unticked ones are built in the repo, but I have not yet explained them back in my own words.

- [x] Price-time priority, and why a trade happens at the resting order's price
- [x] Why prices are integer ticks
- [x] Why matching is single-threaded per symbol
- [x] Why a modify loses its queue position
- [x] Why cancel is O(1): the id index plus an intrusive doubly linked list
- [x] Why interleaved A/B is better than two separate sets of runs
- [ ] How limit, market, cancel and modify differ in this engine
- [ ] What a differential fuzz test proves, and what it cannot prove
- [ ] Why a deterministic sequence counter is better than timestamps here
- [ ] Why percentiles are better than averages for latency
- [ ] Why the benchmark pre-generates its operations, discards warm-up, and keeps a checksum
- [ ] An object pool with a free list, and why addresses must stay stable
- [ ] Open addressing with linear probing, load factor, and backward-shift deletion
- [ ] Why a power-of-two hash table needs a mixing hash
- [ ] A bitmap with a best-price cursor, and `countl_zero` / `countr_zero`
- [ ] Cache lines, and why four 16-byte slots share one
- [ ] Instruction count versus cache misses versus branch mispredicts, and why simulated numbers are not time
- [ ] AddressSanitizer and UndefinedBehaviorSanitizer, and ASan poisoning inside a pool
- [ ] SPSC ring buffer (not built)
- [ ] Memory ordering with acquire and release (not built)
- [ ] False sharing between threads (not built)
- [ ] Replaying real market data (not built)