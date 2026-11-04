#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "lob/naive_matcher.hpp"
#include "lob/order_book.hpp"
#include "lob/order_generator.hpp"
#include "test_util.hpp"

namespace {

using lob::GenConfig;
using lob::NaiveMatcher;
using lob::Op;
using lob::OpKind;
using lob::OrderBook;
using lob::OrderGenerator;
using lob::Side;
using lob::Trade;
using Trades = std::vector<Trade>;

// Operations per seed: FUZZ_OPS if set, otherwise a default sized for a quick test run.
//
// The defaults are small on purpose. The generator picks cancel/modify targets from every id
// ever issued, so almost all of them miss and the book grows roughly linearly with run length
// (about 7000 resting orders after 200k ops). NaiveMatcher scans the whole book per fill, so
// total cost is quadratic: 40k ops/seed already takes 32 s in Debug, and 200k ops/seed 48 s in
// Release. Raise FUZZ_OPS for deeper one-off runs.
uint64_t fuzz_ops() {
  if (const char* env = std::getenv("FUZZ_OPS")) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env && *end == '\0' && v > 0) return v;
  }
#ifdef NDEBUG
  return 100'000;
#else
  return 20'000;
#endif
}

struct Outcome {
  Trades trades;
  bool cancelled = false;  // only meaningful for Cancel
};

template <class M>
Outcome apply(M& m, const Op& op) {
  Outcome out;
  switch (op.kind) {
    case OpKind::Limit:
      out.trades = m.submit_limit(op.id, op.side, op.price, op.qty);
      break;
    case OpKind::Market:
      out.trades = m.submit_market(op.id, op.side, op.qty);
      break;
    case OpKind::Cancel:
      out.cancelled = m.cancel(op.id);
      break;
    case OpKind::Modify:
      out.trades = m.modify(op.id, op.price, op.qty);
      break;
  }
  return out;
}

std::string describe(const Op& op) {
  std::ostringstream os;
  const char* side = op.side == Side::Buy ? "BUY" : "SELL";
  switch (op.kind) {
    case OpKind::Limit:
      os << "LIMIT id=" << op.id << " " << side << " price=" << op.price << " qty=" << op.qty;
      break;
    case OpKind::Market:
      os << "MARKET id=" << op.id << " " << side << " qty=" << op.qty;
      break;
    case OpKind::Cancel:
      os << "CANCEL id=" << op.id;
      break;
    case OpKind::Modify:
      os << "MODIFY id=" << op.id << " price=" << op.price << " qty=" << op.qty;
      break;
  }
  return os.str();
}

std::string describe(const Outcome& out) {
  std::ostringstream os;
  os << "cancelled=" << (out.cancelled ? "true" : "false") << " trades=[";
  for (std::size_t i = 0; i < out.trades.size(); ++i) {
    if (i != 0) os << ", ";
    os << out.trades[i];
  }
  os << "]";
  return os.str();
}

std::string describe(const std::optional<int64_t>& v) {
  return v.has_value() ? std::to_string(*v) : "none";
}

struct Stats {
  uint64_t ops = 0;
  uint64_t trades = 0;
  uint64_t cancel_attempts = 0;
  uint64_t cancel_hits = 0;
};

// Runs n operations through both implementations. Returns an empty string if they agreed
// throughout, otherwise a full description of the first mismatch.
std::string run_seed(uint64_t seed, uint64_t n, Stats& stats) {
  GenConfig cfg;
  cfg.seed = seed;
  OrderGenerator gen(cfg);
  NaiveMatcher naive;
  OrderBook book;
  std::deque<std::pair<uint64_t, Op>> recent;  // last 10 operations

  for (uint64_t i = 0; i < n; ++i) {
    const Op op = gen.next();
    recent.emplace_back(i, op);
    if (recent.size() > 10) recent.pop_front();

    const Outcome a = apply(naive, op);
    const Outcome b = apply(book, op);

    ++stats.ops;
    stats.trades += a.trades.size();
    if (op.kind == OpKind::Cancel) {
      ++stats.cancel_attempts;
      if (a.cancelled) ++stats.cancel_hits;
    }

    std::string why;
    if (a.trades != b.trades) {
      why = "returned trades differ";
    } else if (a.cancelled != b.cancelled) {
      why = "cancel result differs";
    } else if (naive.best_bid() != book.best_bid()) {
      why = "best_bid differs: naive=" + describe(naive.best_bid()) +
            " book=" + describe(book.best_bid());
    } else if (naive.best_ask() != book.best_ask()) {
      why = "best_ask differs: naive=" + describe(naive.best_ask()) +
            " book=" + describe(book.best_ask());
    } else if ((i + 1) % 1000 == 0 || i + 1 == n) {
      for (int64_t p = cfg.price_min; p <= cfg.price_max && why.empty(); ++p) {
        for (Side s : {Side::Buy, Side::Sell}) {
          if (naive.total_qty_at(s, p) != book.total_qty_at(s, p)) {
            why = std::string("total_qty_at differs at ") + (s == Side::Buy ? "BUY" : "SELL") +
                  " price=" + std::to_string(p) +
                  ": naive=" + std::to_string(naive.total_qty_at(s, p)) +
                  " book=" + std::to_string(book.total_qty_at(s, p));
            break;
          }
        }
      }
      if (why.empty() && !book.check_invariants()) why = "OrderBook::check_invariants() failed";
    }

    if (!why.empty()) {
      std::ostringstream os;
      os << "Mismatch: " << why << "\n  seed: " << seed << "\n  operation index: " << i
         << "\n  operation: " << describe(op) << "\n  NaiveMatcher: " << describe(a)
         << "\n  OrderBook:    " << describe(b) << "\n  last " << recent.size()
         << " operations:";
      for (const auto& [idx, past] : recent) os << "\n    #" << idx << " " << describe(past);
      return os.str();
    }
  }
  return {};
}

TEST(Fuzz, NaiveMatcherAndOrderBookAgree) {
  const uint64_t n = fuzz_ops();
  const std::vector<uint64_t> seeds = {1, 2, 3, 42, 12345, 987654321};

  Stats stats;
  const auto start = std::chrono::steady_clock::now();
  for (const uint64_t seed : seeds) {
    const std::string failure = run_seed(seed, n, stats);
    ASSERT_TRUE(failure.empty()) << failure;
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::cout << "[fuzz] " << seeds.size() << " seeds x " << n << " ops = " << stats.ops
            << " ops, " << stats.trades << " trades, cancels hit " << stats.cancel_hits << "/"
            << stats.cancel_attempts << ", " << secs << " s\n";
}

}  // namespace
