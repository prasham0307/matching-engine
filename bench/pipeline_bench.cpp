// Two-thread pipeline: a producer thread pushes pre-generated operations into an SpscQueue and a
// single matcher thread pops them and applies them to one OrderBook. Exactly one thread ever
// touches the book. Reports matcher-side THROUGHPUT only; it does not measure queue latency.
//
//   lob_pipeline [--ops N] [--warmup N] [--seed S] [--capacity C]
//                [--matcher-core M] [--producer-core P]
//                [--variant default|uncached|unpadded|batch32]
//
// The checksum is folded exactly as in lob_latency (shared code in bench_common.hpp), over the
// measured operations only, so the same ops and seed must give the same checksum as lob_latency.
// That is the check that putting a queue and a second thread in front of the book changes nothing.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "lob/order_book.hpp"
#include "lob/order_generator.hpp"
#include "lob/spsc_queue.hpp"

#ifndef LOB_BUILD_TYPE
#define LOB_BUILD_TYPE "unknown"
#endif

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

namespace {

using namespace lob::bench;
using lob::Op;
using lob::OrderBook;
using lob::SpscQueue;
using Clock = std::chrono::steady_clock;

enum class Variant { Default, Uncached, Unpadded, Batch32 };

struct Options {
  uint64_t ops = 5'000'000;
  uint64_t warmup = 200'000;
  uint64_t seed = 1;
  uint64_t capacity = 1024;  // queue slots, a power of two
  int matcher_core = 2;      // the same core lob_latency is pinned to by default in this repo's docs
  int producer_core = 4;     // a different physical core (core 3 would be core 2's SMT sibling)
  Variant variant = Variant::Default;
};

const char* variant_name(Variant v) {
  switch (v) {
    case Variant::Default: return "default (cached counters, padded)";
    case Variant::Uncached: return "uncached counters, padded";
    case Variant::Unpadded: return "cached counters, unpadded";
    case Variant::Batch32: return "cached counters, padded, batch pop of 32";
  }
  return "?";
}

void print_usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--ops N] [--warmup N] [--seed S] [--capacity C]\n"
               "          [--matcher-core M] [--producer-core P]\n"
               "          [--variant default|uncached|unpadded|batch32]\n"
               "  --ops N          measured operations (default 5000000)\n"
               "  --warmup N       warm-up operations, not timed or checksummed (default 200000)\n"
               "  --seed S         generator seed (default 1)\n"
               "  --capacity C     queue slots, a power of two (default 1024)\n"
               "  --matcher-core   core for the matcher thread (default 2)\n"
               "  --producer-core  core for the producer thread (default 4)\n"
               "  --variant        queue layout under test (default: cached counters, padded)\n",
               argv0);
}

bool parse_args(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
      return false;
    }
    const std::string_view value = argv[++i];
    if (arg == "--variant") {
      if (value == "default") opt.variant = Variant::Default;
      else if (value == "uncached") opt.variant = Variant::Uncached;
      else if (value == "unpadded") opt.variant = Variant::Unpadded;
      else if (value == "batch32") opt.variant = Variant::Batch32;
      else {
        std::fprintf(stderr, "error: unknown variant '%s'\n", argv[i]);
        return false;
      }
      continue;
    }
    const auto n = parse_u64(value);
    if (!n) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
    if (arg == "--ops") opt.ops = *n;
    else if (arg == "--warmup") opt.warmup = *n;
    else if (arg == "--seed") opt.seed = *n;
    else if (arg == "--capacity") opt.capacity = *n;
    else if (arg == "--matcher-core") opt.matcher_core = static_cast<int>(*n);
    else if (arg == "--producer-core") opt.producer_core = static_cast<int>(*n);
    else {
      std::fprintf(stderr, "error: unknown option '%s'\n", argv[i - 1]);
      return false;
    }
  }
  if (opt.ops == 0) {
    std::fprintf(stderr, "error: --ops must be greater than 0\n");
    return false;
  }
  return true;
}

inline void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#else
  std::this_thread::yield();
#endif
}

std::string siblings_of(int core) {
  const std::string path =
      "/sys/devices/system/cpu/cpu" + std::to_string(core) + "/topology/thread_siblings_list";
  std::string s = read_file(path.c_str());
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s.empty() ? "unknown" : s;
}

struct RunResult {
  double seconds = 0;
  uint64_t checksum = 0;
  uint64_t trades = 0;
  std::size_t resting = 0;
  uint64_t full_episodes = 0;   // producer: times it found the queue full (a run of failed pushes = 1)
  uint64_t full_polls = 0;      // producer: failed push attempts
  uint64_t empty_episodes = 0;  // matcher: times it found the queue empty
  uint64_t empty_polls = 0;     // matcher: failed pop attempts
  int producer_cpu_start = -1, producer_cpu_end = -1;
  int matcher_cpu_start = -1, matcher_cpu_end = -1;
  bool producer_pin_failed = false;
  bool matcher_pin_failed = false;
};

// What the producer thread reports. Its counters are updated in a spin loop, so they live in its
// own object (and cache line) and are copied into the RunResult after the join, instead of
// sharing a struct with the matcher's counters, which would be false sharing in the harness.
struct alignas(lob::kCacheLineBytes) ProducerReport {
  uint64_t full_episodes = 0;
  uint64_t full_polls = 0;
  int cpu_start = -1;
  int cpu_end = -1;
  bool pin_failed = false;
};

// Runs the whole pipeline once. The calling thread is the matcher. Counters of "queue full" and
// "queue empty" cover only the measured part (operations after the warm-up).
template <class Queue, bool Batch>
RunResult run_pipeline(const Options& opt, const std::vector<Op>& ops) {
  const uint64_t total = ops.size();
  Queue queue(static_cast<std::size_t>(opt.capacity));  // the only allocation the queue makes

  const uint64_t expected_resting = std::max<uint64_t>(1024, (opt.warmup + opt.ops) / 50);
  OrderBook book(static_cast<std::size_t>(expected_resting), 0, OrderBook::kDefaultWindowTicks);
  Result result;
  result.trades.reserve(64);

  RunResult r;
  ProducerReport producer_report;
  std::atomic<bool> producer_ready{false};
  std::atomic<bool> go{false};

  std::thread producer([&] {
    ProducerReport local;  // counters stay in this thread's registers and stack while it runs
    local.pin_failed = !pin_to_core(opt.producer_core);
    local.cpu_start = sched_getcpu();
    producer_ready.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();

    bool in_full = false;
    for (uint64_t i = 0; i < total; ++i) {
      const bool measured = i >= opt.warmup;
      while (!queue.try_push(ops[i])) {
        if (measured) {
          ++local.full_polls;
          if (!in_full) {
            ++local.full_episodes;
            in_full = true;
          }
        }
        cpu_relax();
      }
      in_full = false;
    }
    local.cpu_end = sched_getcpu();
    producer_report = local;  // written once, read by the matcher thread only after join()
  });

  r.matcher_pin_failed = !pin_to_core(opt.matcher_core);
  r.matcher_cpu_start = sched_getcpu();
  while (!producer_ready.load(std::memory_order_acquire)) cpu_relax();

  uint64_t processed = 0;
  uint64_t checksum = 0;
  uint64_t trades = 0;
  uint64_t empty_polls = 0;     // thread-local, like the producer's counters
  uint64_t empty_episodes = 0;
  bool in_empty = false;
  std::array<Op, 32> batch;
  Clock::time_point t0 = Clock::now();

  go.store(true, std::memory_order_release);
  if (opt.warmup == 0) t0 = Clock::now();

  while (processed < total) {
    std::size_t got = 0;
    if constexpr (Batch) {
      got = queue.try_pop_batch(batch.data(), batch.size());
    } else {
      if (queue.try_pop(batch[0])) got = 1;
    }
    if (got == 0) {
      if (processed >= opt.warmup) {
        ++empty_polls;
        if (!in_empty) {
          ++empty_episodes;
          in_empty = true;
        }
      }
      cpu_relax();
      continue;
    }
    in_empty = false;
    for (std::size_t k = 0; k < got; ++k) {
      apply(book, batch[k], result);
      if (processed >= opt.warmup) {
        checksum = fold(checksum, result);
        trades += result.trades.size();
      }
      ++processed;
      if (processed == opt.warmup) t0 = Clock::now();  // the clock starts after the last warm-up op
    }
  }
  const Clock::time_point t1 = Clock::now();
  r.matcher_cpu_end = sched_getcpu();
  producer.join();

  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.empty_episodes = empty_episodes;
  r.empty_polls = empty_polls;
  r.full_episodes = producer_report.full_episodes;
  r.full_polls = producer_report.full_polls;
  r.producer_cpu_start = producer_report.cpu_start;
  r.producer_cpu_end = producer_report.cpu_end;
  r.producer_pin_failed = producer_report.pin_failed;
  r.checksum = checksum;
  r.trades = trades;
  r.resting = book.resting_orders();
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt)) {
    print_usage(argv[0]);
    return 2;
  }
  if (kDebugBuild) {
    std::fprintf(stderr,
                 "WARNING: this binary was built without NDEBUG (Debug, sanitizers on).\n"
                 "         Throughput numbers from it are meaningless. Build with\n"
                 "         -DCMAKE_BUILD_TYPE=Release and run again. Exiting.\n");
    return 1;
  }

  // Two threads need two cores. Refuse rather than quietly run both on one core, which would make
  // each spin while the other cannot run and produce numbers that mean nothing.
  const unsigned cores = std::thread::hardware_concurrency();
  if (cores < 2) {
    std::fprintf(stderr, "error: this machine reports %u core(s); the pipeline needs at least 2. Not running.\n",
                 cores);
    return 2;
  }
  if (opt.matcher_core == opt.producer_core || opt.matcher_core < 0 || opt.producer_core < 0 ||
      static_cast<unsigned>(opt.matcher_core) >= cores || static_cast<unsigned>(opt.producer_core) >= cores) {
    std::fprintf(stderr,
                 "error: matcher core %d and producer core %d must be two different cores below %u\n",
                 opt.matcher_core, opt.producer_core, cores);
    return 2;
  }
  const std::string matcher_siblings = siblings_of(opt.matcher_core);
  const std::string producer_siblings = siblings_of(opt.producer_core);
  const bool smt_siblings = matcher_siblings == producer_siblings && matcher_siblings != "unknown";

  // Pre-generate every operation before any timing, the same stream lob_latency uses.
  lob::GenConfig cfg;
  cfg.seed = opt.seed;
  lob::OrderGenerator gen(cfg);
  std::vector<Op> ops;
  ops.reserve(opt.warmup + opt.ops);
  for (uint64_t i = 0; i < opt.warmup + opt.ops; ++i) ops.push_back(gen.next());

  RunResult r;
  switch (opt.variant) {
    case Variant::Default:
      r = run_pipeline<SpscQueue<Op, true, true>, false>(opt, ops);
      break;
    case Variant::Uncached:
      r = run_pipeline<SpscQueue<Op, false, true>, false>(opt, ops);
      break;
    case Variant::Unpadded:
      r = run_pipeline<SpscQueue<Op, true, false>, false>(opt, ops);
      break;
    case Variant::Batch32:
      r = run_pipeline<SpscQueue<Op, true, true>, true>(opt, ops);
      break;
  }

  const double ops_per_sec = static_cast<double>(opt.ops) / r.seconds;
  std::printf("== lob_pipeline ==\n");
  std::printf("machine:\n");
  std::printf("  cpu:       %s\n", cpu_model().c_str());
  std::printf("  cores:     %u\n", cores);
  std::printf("  build:     %s (NDEBUG)\n", LOB_BUILD_TYPE);
  std::printf("  wsl:       %s\n", running_under_wsl() ? "yes (timings are noisy; prefer native Linux)" : "no");
  std::printf("threads:\n");
  std::printf("  matcher:   pinned to core %d (siblings %s); ran on core %d at start, %d at end\n",
              opt.matcher_core, matcher_siblings.c_str(), r.matcher_cpu_start, r.matcher_cpu_end);
  std::printf("  producer:  pinned to core %d (siblings %s); ran on core %d at start, %d at end\n",
              opt.producer_core, producer_siblings.c_str(), r.producer_cpu_start, r.producer_cpu_end);
  if (smt_siblings) std::printf("  WARNING:   the two cores are SMT siblings of one physical core\n");
  if (r.producer_pin_failed || r.matcher_pin_failed) {
    std::printf("  WARNING:   pinning a thread failed; the cores above may be wrong\n");
  }
  std::printf("config:\n");
  std::printf("  variant %s, queue capacity %llu slots\n", variant_name(opt.variant),
              static_cast<unsigned long long>(opt.capacity));
  std::printf("  seed %llu, warmup %llu ops (discarded), measured %llu ops\n",
              static_cast<unsigned long long>(opt.seed), static_cast<unsigned long long>(opt.warmup),
              static_cast<unsigned long long>(opt.ops));
  std::printf("results (matcher-side throughput over the measured ops; no queue latency is measured):\n");
  std::printf("  throughput      %.0f ops/sec (wall %.3f s)\n", ops_per_sec, r.seconds);
  std::printf("  producer found the queue full   %llu times (%llu failed pushes)\n",
              static_cast<unsigned long long>(r.full_episodes), static_cast<unsigned long long>(r.full_polls));
  std::printf("  matcher found the queue empty   %llu times (%llu failed pops)\n",
              static_cast<unsigned long long>(r.empty_episodes), static_cast<unsigned long long>(r.empty_polls));
  std::printf("  trades          %llu\n", static_cast<unsigned long long>(r.trades));
  std::printf("  resting orders  %zu (at end)\n", r.resting);
  std::printf("  checksum        %016llx\n", static_cast<unsigned long long>(r.checksum));
  std::printf(
      "RESULT variant=%s ops_per_sec=%.0f full_episodes=%llu empty_episodes=%llu full_polls=%llu "
      "empty_polls=%llu trades=%llu resting=%zu checksum=%016llx\n",
      opt.variant == Variant::Default ? "default"
      : opt.variant == Variant::Uncached ? "uncached"
      : opt.variant == Variant::Unpadded ? "unpadded"
                                         : "batch32",
      ops_per_sec, static_cast<unsigned long long>(r.full_episodes),
      static_cast<unsigned long long>(r.empty_episodes), static_cast<unsigned long long>(r.full_polls),
      static_cast<unsigned long long>(r.empty_polls), static_cast<unsigned long long>(r.trades),
      r.resting, static_cast<unsigned long long>(r.checksum));
  return 0;
}
