// lob_replay: replay a text file of order events through the MatchingEngine, or generate one.
//
//   lob_replay INPUT [--trades PATH|-] [--verify] [--resting N] [--base B] [--window W]
//   lob_replay --generate OUTPUT --events N --symbols S [--seed X]
//
// Exit status: 0 success, 1 --verify found a difference, 2 bad usage or a malformed input line,
// 3 an input or output file could not be opened or written.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "lob/replay.hpp"

namespace {

void print_usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s INPUT [--trades PATH|-] [--verify] [--resting N] [--base B] [--window W]\n"
               "       %s --generate OUTPUT --events N --symbols S [--seed X]\n"
               "\n"
               "Replay mode reads one event per line (CSV):\n"
               "  LIMIT,<id>,<symbol>,<BUY|SELL>,<price_ticks>,<qty>\n"
               "  MARKET,<id>,<symbol>,<BUY|SELL>,<qty>\n"
               "  CANCEL,<id>\n"
               "  MODIFY,<id>,<new_price_ticks>,<new_qty>\n"
               "Lines starting with # and blank lines are ignored. A malformed line stops the run.\n"
               "\n"
               "  --trades PATH   write the trade log to PATH ('-' for stdout, then the summary\n"
               "                  goes to stderr). One line per trade: maker,taker,symbol,price,qty\n"
               "  --verify        also run every event through NaiveMatcher and stop at the first\n"
               "                  difference (slow: NaiveMatcher scans its whole book)\n"
               "  --resting N     expected resting orders, to size the engine (default 65536)\n"
               "  --base B        first tick of each book's price window (default 0)\n"
               "  --window W      ticks in the window (default 4096); other prices are slower\n"
               "  --generate      write a generated event file instead of replaying one\n",
               argv0, argv0);
}

template <class T>
bool parse_number(std::string_view s, T& out) {
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return !s.empty() && ec == std::errc{} && ptr == end;
}

struct Args {
  std::string input;
  std::string generate_path;
  std::string trades_path;
  bool verify = false;
  bool generate = false;
  uint64_t events = 0;
  uint64_t symbols = 0;
  uint64_t seed = 1;
  uint64_t resting = 65536;
  int64_t base = 0;
  uint64_t window = lob::OrderBook::kDefaultWindowTicks;
};

// Returns false after printing the reason.
bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](const char* what) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs %s\n", argv[i], what);
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };
    const auto number = [&](auto& out, const char* name) {
      const auto v = value("a number");
      if (!v) return false;
      if (!parse_number(*v, out)) {
        std::fprintf(stderr, "error: bad value '%.*s' for %s\n", static_cast<int>(v->size()),
                     v->data(), name);
        return false;
      }
      return true;
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "--verify") {
      a.verify = true;
    } else if (arg == "--generate") {
      const auto v = value("an output path");
      if (!v) return false;
      a.generate = true;
      a.generate_path = std::string(*v);
    } else if (arg == "--trades") {
      const auto v = value("a path or '-'");
      if (!v) return false;
      a.trades_path = std::string(*v);
    } else if (arg == "--events") {
      if (!number(a.events, "--events")) return false;
    } else if (arg == "--symbols") {
      if (!number(a.symbols, "--symbols")) return false;
    } else if (arg == "--seed") {
      if (!number(a.seed, "--seed")) return false;
    } else if (arg == "--resting") {
      if (!number(a.resting, "--resting")) return false;
    } else if (arg == "--base") {
      if (!number(a.base, "--base")) return false;
    } else if (arg == "--window") {
      if (!number(a.window, "--window")) return false;
    } else if (!arg.empty() && arg.front() == '-' && arg != "-") {
      std::fprintf(stderr, "error: unknown option '%.*s'\n", static_cast<int>(arg.size()), arg.data());
      return false;
    } else if (a.input.empty()) {
      a.input = std::string(arg);
    } else {
      std::fprintf(stderr, "error: more than one input file given\n");
      return false;
    }
  }
  if (a.generate) {
    if (!a.input.empty()) {
      std::fprintf(stderr, "error: --generate writes a file; it does not take an input file\n");
      return false;
    }
    if (a.events == 0 || a.symbols == 0 || a.symbols > 0xFFFFFFFFu) {
      std::fprintf(stderr, "error: --generate needs --events N (N > 0) and --symbols S (S > 0)\n");
      return false;
    }
    if (a.verify || !a.trades_path.empty()) {
      std::fprintf(stderr, "error: --verify and --trades apply to replay mode, not --generate\n");
      return false;
    }
  } else if (a.input.empty()) {
    std::fprintf(stderr, "error: no input file given\n");
    return false;
  }
  return true;
}

int run_generate(const Args& a) {
  std::ofstream out(a.generate_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "error: cannot open '%s' for writing\n", a.generate_path.c_str());
    return 3;
  }
  lob::replay::generate_events(out, a.events, static_cast<uint32_t>(a.symbols), a.seed);
  out.flush();
  if (!out) {
    std::fprintf(stderr, "error: could not write '%s'\n", a.generate_path.c_str());
    return 3;
  }
  std::printf("wrote %llu events over %llu symbols (seed %llu) to %s\n",
              static_cast<unsigned long long>(a.events), static_cast<unsigned long long>(a.symbols),
              static_cast<unsigned long long>(a.seed), a.generate_path.c_str());
  return 0;
}

void print_opt(std::FILE* f, const std::optional<int64_t>& v) {
  if (v) {
    std::fprintf(f, "%lld", static_cast<long long>(*v));
  } else {
    std::fputs("none", f);
  }
}

int run_replay(const Args& a) {
  std::ifstream in(a.input, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: cannot open '%s'\n", a.input.c_str());
    return 3;
  }

  std::ofstream trade_file;
  std::ostream* trade_log = nullptr;
  std::FILE* summary = stdout;
  if (a.trades_path == "-") {
    trade_log = &std::cout;
    summary = stderr;  // keep stdout for the trade log alone
  } else if (!a.trades_path.empty()) {
    trade_file.open(a.trades_path, std::ios::binary);
    if (!trade_file) {
      std::fprintf(stderr, "error: cannot open '%s' for writing\n", a.trades_path.c_str());
      return 3;
    }
    trade_log = &trade_file;
  }

  lob::replay::ReplayOptions options;
  options.verify = a.verify;
  options.trade_log = trade_log;
  options.engine.expected_resting_orders = a.resting;
  options.engine.expected_resting_per_symbol = a.resting / 4 + 1024;
  options.engine.window_base = a.base;
  options.engine.window_ticks = static_cast<std::size_t>(a.window);

  const auto start = std::chrono::steady_clock::now();
  const lob::replay::ReplayResult r = lob::replay::replay(in, options);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (trade_file.is_open()) trade_file.close();

  if (r.failure == lob::replay::Failure::Malformed) {
    std::fprintf(stderr, "%s: line %llu: %s\n", a.input.c_str(),
                 static_cast<unsigned long long>(r.line), r.message.c_str());
    std::fprintf(stderr, "events processed before the error: %llu; no summary because the run is incomplete\n",
                 static_cast<unsigned long long>(r.events));
    return 2;
  }
  if (r.failure == lob::replay::Failure::Mismatch) {
    std::fprintf(stderr, "%s: line %llu: %s\n", a.input.c_str(),
                 static_cast<unsigned long long>(r.line), r.message.c_str());
    return 1;
  }
  if (r.failure == lob::replay::Failure::Io) {
    std::fprintf(stderr, "error: %s\n", r.message.c_str());
    return 3;
  }

  std::fprintf(summary, "events processed: %llu\n", static_cast<unsigned long long>(r.events));
  std::fprintf(summary, "lines read: %llu (comments and blank lines: %llu)\n",
               static_cast<unsigned long long>(r.lines),
               static_cast<unsigned long long>(r.lines - r.events));
  std::fprintf(summary, "trades: %llu\n", static_cast<unsigned long long>(r.trades));
  std::fprintf(summary, "rejected orders (quantity 0, or id already resting): %llu\n",
               static_cast<unsigned long long>(r.rejected_orders));
  std::fprintf(summary, "cancels of an id that was not resting: %llu\n",
               static_cast<unsigned long long>(r.missed_cancels));
  std::fprintf(summary, "modifies of an id that was not resting: %llu\n",
               static_cast<unsigned long long>(r.missed_modifies));
  for (const auto& s : r.symbols) {
    std::fprintf(summary, "symbol %u: best bid ", s.symbol);
    print_opt(summary, s.best_bid);
    std::fputs(", best ask ", summary);
    print_opt(summary, s.best_ask);
    std::fprintf(summary, ", %zu resting\n", s.resting);
  }
  std::fprintf(summary, "trade log hash (FNV-1a 64): 0x%016llx\n",
               static_cast<unsigned long long>(r.trade_hash));
  if (a.verify) std::fputs("verify: passed, every event matched NaiveMatcher\n", summary);
  std::fprintf(summary, "global id table resizes: %zu\n", r.id_table_resizes);
  std::fprintf(summary,
               "elapsed: %.3f s, %.0f events/s (includes parsing%s%s; not comparable to lob_latency)\n",
               secs, secs > 0 ? static_cast<double>(r.events) / secs : 0.0,
               trade_log != nullptr ? " and trade log output" : " and trade log formatting",
               a.verify ? " and NaiveMatcher verification" : "");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    print_usage(argv[0]);
    return 2;
  }
  return args.generate ? run_generate(args) : run_replay(args);
}
