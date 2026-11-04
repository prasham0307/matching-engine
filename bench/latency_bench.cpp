// Per-operation latency harness for OrderBook.
//
// Method: all operations are generated before any timing starts. A warm-up prefix is run on a
// fresh book and its timings are discarded; each measured operation is then timed on its own
// with steady_clock into a preallocated vector. The trade vector an operation returns is
// created inside the timed region (the engine's allocation is part of what is measured) and
// destroyed after it. Reports percentiles, not averages.
#include <sched.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/order_generator.hpp"

#if defined(__clang__)
#define LOB_COMPILER_NAME "clang "
#else
#define LOB_COMPILER_NAME "g++ "
#endif

// Profiling builds only (-DLOB_CALLGRIND): bracket the engine's work with Callgrind collect
// toggles so a `valgrind --tool=callgrind --collect-atstart=no` profile contains only the
// order book (the operation and the destruction of its returned trades), not the harness setup.
// Without the define these expand to nothing and the code is identical to the normal build.
#ifdef LOB_CALLGRIND
#include <valgrind/callgrind.h>
#define LOB_PROFILE_TOGGLE() CALLGRIND_TOGGLE_COLLECT
#else
#define LOB_PROFILE_TOGGLE() ((void)0)
#endif

#ifndef LOB_BUILD_TYPE
#define LOB_BUILD_TYPE "unknown"
#endif

namespace {

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

using Clock = std::chrono::steady_clock;
using lob::Op;
using lob::OpKind;
using lob::OrderBook;
using lob::Trade;

struct Options {
  uint64_t ops = 5'000'000;
  uint64_t warmup = 200'000;
  uint64_t seed = 1;
  std::optional<int> pin_core;
  std::string dump_path;
};

void print_usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--ops N] [--warmup N] [--seed S] [--pin-core C] [--dump PATH]\n"
               "  --ops N        measured operations (default 5000000)\n"
               "  --warmup N     warm-up operations, timings discarded (default 200000)\n"
               "  --seed S       generator seed (default 1)\n"
               "  --pin-core C   pin the thread to core C (default: no pinning)\n"
               "  --dump PATH    write raw per-op latencies in ns, one per line\n",
               argv0);
}

std::optional<uint64_t> parse_u64(std::string_view s) {
  uint64_t v = 0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (s.empty() || ec != std::errc{} || ptr != end) return std::nullopt;
  return v;
}

// Returns false (after printing why) if the arguments are unusable.
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
    if (arg == "--dump") {
      opt.dump_path = std::string(value);
      continue;
    }
    const auto n = parse_u64(value);
    if (!n) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
    if (arg == "--ops") {
      opt.ops = *n;
    } else if (arg == "--warmup") {
      opt.warmup = *n;
    } else if (arg == "--seed") {
      opt.seed = *n;
    } else if (arg == "--pin-core") {
      opt.pin_core = static_cast<int>(*n);
    } else {
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

std::string read_file(const char* path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string cpu_model() {
  std::istringstream in(read_file("/proc/cpuinfo"));
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("model name", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos && colon + 2 <= line.size()) return line.substr(colon + 2);
    }
  }
  return "unknown";
}

bool running_under_wsl() {
  std::string v = read_file("/proc/version");
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v.find("microsoft") != std::string::npos || v.find("wsl") != std::string::npos;
}

bool pin_to_core(int core) {
  if (core < 0 || core >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// Median cost of two back-to-back steady_clock::now() calls, in ns.
double measure_timer_overhead_ns() {
  std::vector<int64_t> samples(1'000'000);
  for (int64_t& s : samples) {
    const auto a = Clock::now();
    const auto b = Clock::now();
    s = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
  }
  const auto mid = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2);
  std::nth_element(samples.begin(), mid, samples.end());
  return static_cast<double>(*mid);
}

struct Result {
  std::vector<Trade> trades;
  bool cancelled = false;
};

Result apply(OrderBook& book, const Op& op) {
  Result r;
  switch (op.kind) {
    case OpKind::Limit:
      r.trades = book.submit_limit(op.id, op.side, op.price, op.qty);
      break;
    case OpKind::Market:
      r.trades = book.submit_market(op.id, op.side, op.qty);
      break;
    case OpKind::Cancel:
      r.cancelled = book.cancel(op.id);
      break;
    case OpKind::Modify:
      r.trades = book.modify(op.id, op.price, op.qty);
      break;
  }
  return r;
}

// Order-sensitive so a change in matching order or results changes the checksum.
uint64_t fold(uint64_t checksum, const Result& r) {
  for (const Trade& t : r.trades) {
    checksum = (checksum + static_cast<uint64_t>(t.price) * t.qty) ^ t.makerId ^ (t.takerId << 32);
  }
  if (r.cancelled) checksum ^= 0x9e3779b97f4a7c15ULL;
  return checksum;
}

// Nearest-rank percentile of an ascending-sorted vector.
uint64_t percentile(const std::vector<uint64_t>& sorted, double pct) {
  const double rank = std::ceil(pct / 100.0 * static_cast<double>(sorted.size()));
  const std::size_t idx = rank < 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
  return sorted[std::min(idx, sorted.size() - 1)];
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
                 "         Latency numbers from it are meaningless. Build with\n"
                 "         -DCMAKE_BUILD_TYPE=Release and run again. Exiting.\n");
    return 1;
  }

  if (opt.pin_core && !pin_to_core(*opt.pin_core)) {
    std::fprintf(stderr, "error: could not pin to core %d\n", *opt.pin_core);
    return 1;
  }
  const int cpu_before = sched_getcpu();

  // 1. Generate everything before any timing.
  lob::GenConfig cfg;
  cfg.seed = opt.seed;
  lob::OrderGenerator gen(cfg);
  std::vector<Op> ops;
  ops.reserve(opt.warmup + opt.ops);
  uint64_t max_id = 0;
  for (uint64_t i = 0; i < opt.warmup + opt.ops; ++i) {
    ops.push_back(gen.next());
    if (ops.back().kind == OpKind::Limit || ops.back().kind == OpKind::Market) {
      max_id = std::max(max_id, ops.back().id);
    }
  }

  const double timer_overhead_ns = measure_timer_overhead_ns();

  // 2. Warm-up on a fresh book, timings discarded.
  OrderBook book;
  uint64_t sink = 0;
  for (uint64_t i = 0; i < opt.warmup; ++i) sink = fold(sink, apply(book, ops[i]));

  // 3. Measured loop. latencies is preallocated; nothing in the loop allocates in the harness.
  std::vector<uint64_t> latencies(opt.ops);
  uint64_t checksum = 0;
  uint64_t trade_count = 0;
  const auto loop_start = Clock::now();
  for (uint64_t i = 0; i < opt.ops; ++i) {
    const Op& op = ops[opt.warmup + i];
    const auto t0 = Clock::now();
    LOB_PROFILE_TOGGLE();
    Result r = apply(book, op);
    LOB_PROFILE_TOGGLE();
    const auto t1 = Clock::now();
    latencies[i] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    checksum = fold(checksum, r);
    trade_count += r.trades.size();
#ifdef LOB_CALLGRIND
    LOB_PROFILE_TOGGLE();
    { const Result dead = std::move(r); }  // include freeing the trade vector in the profile
    LOB_PROFILE_TOGGLE();
#endif
  }
  const auto loop_end = Clock::now();
  const int cpu_after = sched_getcpu();
  const double wall_s = std::chrono::duration<double>(loop_end - loop_start).count();

  uint64_t resting = 0;
  for (uint64_t id = 1; id <= max_id; ++id) {
    if (book.is_resting(id)) ++resting;
  }

  if (!opt.dump_path.empty()) {
    std::ofstream out(opt.dump_path);
    for (const uint64_t ns : latencies) out << ns << '\n';
  }

  std::vector<uint64_t> sorted = latencies;
  std::sort(sorted.begin(), sorted.end());
  const uint64_t p50 = percentile(sorted, 50.0);
  const uint64_t p90 = percentile(sorted, 90.0);
  const uint64_t p99 = percentile(sorted, 99.0);
  const uint64_t p999 = percentile(sorted, 99.9);
  const uint64_t pmax = sorted.back();
  const double ops_per_sec = static_cast<double>(opt.ops) / wall_s;

  std::printf("== lob_latency (OrderBook) ==\n");
  std::printf("machine:\n");
  std::printf("  cpu:       %s\n", cpu_model().c_str());
  std::printf("  cores:     %u\n", std::thread::hardware_concurrency());
  std::printf("  compiler:  %s%s\n", LOB_COMPILER_NAME, __VERSION__);
  std::printf("  build:     %s (NDEBUG)\n", LOB_BUILD_TYPE);
  if (opt.pin_core) {
    std::printf("  pinned:    yes, core %d (running on core %d before, %d after)\n", *opt.pin_core,
                cpu_before, cpu_after);
  } else {
    std::printf("  pinned:    no (running on core %d before, %d after)\n", cpu_before, cpu_after);
  }
  std::printf("  wsl:       %s\n",
              running_under_wsl() ? "yes (timings are noisy; prefer native Linux)" : "no");
  std::printf("config:\n");
  std::printf("  seed %llu, warmup %llu ops (discarded), measured %llu ops\n",
              static_cast<unsigned long long>(opt.seed),
              static_cast<unsigned long long>(opt.warmup),
              static_cast<unsigned long long>(opt.ops));
  std::printf("results (ns per op, raw, timer overhead not subtracted):\n");
  std::printf("  p50    %llu\n", static_cast<unsigned long long>(p50));
  std::printf("  p90    %llu\n", static_cast<unsigned long long>(p90));
  std::printf("  p99    %llu\n", static_cast<unsigned long long>(p99));
  std::printf("  p99.9  %llu\n", static_cast<unsigned long long>(p999));
  std::printf("  max    %llu\n", static_cast<unsigned long long>(pmax));
  std::printf("  throughput      %.0f ops/sec (wall %.3f s)\n", ops_per_sec, wall_s);
  std::printf("  timer overhead  %.0f ns (median of 1,000,000 back-to-back now() pairs)\n",
              timer_overhead_ns);
  std::printf("  trades          %llu\n", static_cast<unsigned long long>(trade_count));
  std::printf("  resting orders  %llu (at end)\n", static_cast<unsigned long long>(resting));
  std::printf("  checksum        %016llx\n", static_cast<unsigned long long>(checksum));
  std::printf(
      "RESULT p50=%llu p90=%llu p99=%llu p999=%llu max=%llu ops_per_sec=%.0f timer_ns=%.0f "
      "trades=%llu resting=%llu checksum=%016llx\n",
      static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p90),
      static_cast<unsigned long long>(p99), static_cast<unsigned long long>(p999),
      static_cast<unsigned long long>(pmax), ops_per_sec, timer_overhead_ns,
      static_cast<unsigned long long>(trade_count), static_cast<unsigned long long>(resting),
      static_cast<unsigned long long>(checksum));

  // Keep the warm-up results observable so none of that work can be elided.
  if (sink == 0x1234567890abcdefULL) std::puts("(sink)");
  return 0;
}
