#include <gtest/gtest.h>

#include <algorithm>
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
// NaiveMatcher scans the whole book on every fill, so total cost grows quadratically with run
// length (the book keeps growing, just more slowly than it used to, about 4900 resting orders
// after 300k ops). Measured on the dev machine with 6 seeds: Debug 40k ops/seed takes 19 s
// (80k takes 66 s), Release 250k ops/seed takes about 20 s (300k takes 35 s). Raise FUZZ_OPS
// for deeper one-off runs.
uint64_t fuzz_ops() {
  if (const char* env = std::getenv("FUZZ_OPS")) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env && *end == '\0' && v > 0) return v;
  }
#ifdef NDEBUG
  return 250'000;
#else
  return 40'000;
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

// OrderBook through the hot-path API, with one buffer that lives for the whole run. If any call
// failed to clear it, stale trades from an earlier operation would show up as a mismatch.
Outcome apply(OrderBook& m, const Op& op, Trades& buffer) {
  Outcome out;
  switch (op.kind) {
    case OpKind::Limit:
      m.submit_limit(op.id, op.side, op.price, op.qty, buffer);
      out.trades = buffer;
      break;
    case OpKind::Market:
      m.submit_market(op.id, op.side, op.qty, buffer);
      out.trades = buffer;
      break;
    case OpKind::Cancel:
      out.cancelled = m.cancel(op.id);
      break;  // buffer deliberately left dirty
    case OpKind::Modify:
      m.modify(op.id, op.price, op.qty, buffer);
      out.trades = buffer;
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
  uint64_t cancel_hits = 0;   // the id was resting and got cancelled
  uint64_t modify_attempts = 0;
  uint64_t modify_hits = 0;   // the id was resting when the modify arrived
  uint64_t max_resting = 0;   // peak number of resting orders seen
  uint64_t out_of_window = 0; // orders that rested at an out-of-window price (the slow path)
};

// Minimum hit rates, set at roughly half of what the generator measured when they were
// chosen (about 17 percent for both). If a generator change drops cancels/modifies back to
// mostly missing, the test fails instead of silently covering less.
constexpr double kMinCancelHitRate = 0.08;
constexpr double kMinModifyHitRate = 0.08;
// Rates are only asserted once there are enough attempts for them to be meaningful.
constexpr uint64_t kMinAttemptsForRateCheck = 1000;

double rate(uint64_t hits, uint64_t attempts) {
  return attempts == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(attempts);
}

// One fuzz scenario: how orders are generated and how the OrderBook's price window is set.
struct FuzzConfig {
  const char* name;
  GenConfig gen;
  int64_t book_base;
  std::size_t book_window;
  double ops_scale;         // fraction of fuzz_ops() run per seed (wide books make NaiveMatcher slow)
  bool forbid_out_of_window;
  double min_out_of_window_per_op;  // guard: the fallback path must really be exercised
};

// The prices at which to compare total_qty_at on both implementations. For a narrow band that is
// every price; otherwise the prices touched recently, the neighbourhood of the best prices, and a
// sample of the levels the book itself reports (NaiveMatcher cannot list its levels, and scanning
// a wide band would make this check dominate the run time).
std::vector<int64_t> probe_prices(const GenConfig& cfg, const NaiveMatcher& naive,
                                  const OrderBook& book, const std::deque<int64_t>& recent_prices) {
  std::vector<int64_t> probes;
  if (cfg.far_price_pct == 0 && cfg.price_max - cfg.price_min <= 64) {
    for (int64_t p = cfg.price_min; p <= cfg.price_max; ++p) probes.push_back(p);
    return probes;
  }
  probes.assign(recent_prices.begin(), recent_prices.end());
  for (const auto& best : {naive.best_bid(), naive.best_ask()}) {
    if (!best) continue;
    for (int64_t d = -2; d <= 2; ++d) probes.push_back(*best + d);
  }
  for (const Side side : {Side::Buy, Side::Sell}) {
    const auto levels = book.depth(side);
    const std::size_t step = std::max<std::size_t>(1, levels.size() / 100);
    for (std::size_t i = 0; i < levels.size(); i += step) probes.push_back(levels[i].first);
  }
  return probes;
}

// depth() must list strictly improving prices, best first, with no empty level.
bool depth_is_well_formed(const OrderBook& book) {
  const auto bids = book.depth(Side::Buy);
  const auto asks = book.depth(Side::Sell);
  for (std::size_t i = 0; i < bids.size(); ++i) {
    if (bids[i].second == 0 || (i > 0 && bids[i].first >= bids[i - 1].first)) return false;
  }
  for (std::size_t i = 0; i < asks.size(); ++i) {
    if (asks[i].second == 0 || (i > 0 && asks[i].first <= asks[i - 1].first)) return false;
  }
  return true;
}

// Runs n operations through both implementations. Returns an empty string if they agreed
// throughout, otherwise a full description of the first mismatch.
std::string run_seed(uint64_t seed, uint64_t n, const FuzzConfig& fc, Stats& stats) {
  GenConfig cfg = fc.gen;
  cfg.seed = seed;
  OrderGenerator gen(cfg);
  NaiveMatcher naive;
  OrderBook book(1024, fc.book_base, fc.book_window);
  Trades buffer;                               // reused by every OrderBook call
  std::deque<std::pair<uint64_t, Op>> recent;  // last 10 operations
  std::deque<int64_t> recent_prices;           // last prices submitted, for probing

  for (uint64_t i = 0; i < n; ++i) {
    const Op op = gen.next();
    recent.emplace_back(i, op);
    if (recent.size() > 10) recent.pop_front();
    if (op.kind == OpKind::Limit || op.kind == OpKind::Modify) {
      recent_prices.push_back(op.price);
      if (recent_prices.size() > 96) recent_prices.pop_front();
    }

    // A modify returns no trades both when rejected and when it rests quietly, so whether it
    // hit has to be read off the book before the operation runs.
    const bool was_resting = naive.is_resting(op.id);

    const Outcome a = apply(naive, op);
    const Outcome b = apply(book, op, buffer);

    ++stats.ops;
    stats.trades += a.trades.size();
    stats.max_resting = std::max<uint64_t>(stats.max_resting, naive.resting_count());
    if (op.kind == OpKind::Cancel) {
      ++stats.cancel_attempts;
      if (a.cancelled) ++stats.cancel_hits;
    } else if (op.kind == OpKind::Modify) {
      ++stats.modify_attempts;
      if (was_resting) ++stats.modify_hits;
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
      for (const int64_t p : probe_prices(cfg, naive, book, recent_prices)) {
        for (const Side s : {Side::Buy, Side::Sell}) {
          if (naive.total_qty_at(s, p) != book.total_qty_at(s, p)) {
            why = std::string("total_qty_at differs at ") + (s == Side::Buy ? "BUY" : "SELL") +
                  " price=" + std::to_string(p) +
                  ": naive=" + std::to_string(naive.total_qty_at(s, p)) +
                  " book=" + std::to_string(book.total_qty_at(s, p));
            break;
          }
        }
        if (!why.empty()) break;
      }
      if (why.empty() && !depth_is_well_formed(book)) why = "OrderBook::depth() is malformed";
      if (why.empty() && !book.check_invariants()) why = "OrderBook::check_invariants() failed";
    }

    if (!why.empty()) {
      std::ostringstream os;
      os << "Mismatch (" << fc.name << "): " << why << "\n  seed: " << seed
         << "\n  operation index: " << i << "\n  operation: " << describe(op)
         << "\n  NaiveMatcher: " << describe(a) << "\n  OrderBook:    " << describe(b)
         << "\n  last " << recent.size() << " operations:";
      for (const auto& [idx, past] : recent) os << "\n    #" << idx << " " << describe(past);
      return os.str();
    }
  }
  stats.out_of_window += book.out_of_window_orders();
  return {};
}

void run_config(const FuzzConfig& fc) {
  const uint64_t n = std::max<uint64_t>(
      1000, static_cast<uint64_t>(static_cast<double>(fuzz_ops()) * fc.ops_scale));
  const std::vector<uint64_t> seeds = {1, 2, 3, 42, 12345, 987654321};

  Stats stats;
  const auto start = std::chrono::steady_clock::now();
  for (const uint64_t seed : seeds) {
    const std::string failure = run_seed(seed, n, fc, stats);
    ASSERT_TRUE(failure.empty()) << failure;
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const double cancel_rate = rate(stats.cancel_hits, stats.cancel_attempts);
  const double modify_rate = rate(stats.modify_hits, stats.modify_attempts);
  const double oow_per_op = static_cast<double>(stats.out_of_window) / static_cast<double>(stats.ops);
  std::cout << "[fuzz] " << fc.name << ": " << seeds.size() << " seeds x " << n << " ops = "
            << stats.ops << " ops, " << stats.trades << " trades, " << secs << " s\n"
            << "[fuzz] " << fc.name << ": cancel hit rate " << 100.0 * cancel_rate << "% ("
            << stats.cancel_hits << "/" << stats.cancel_attempts << "), modify hit rate "
            << 100.0 * modify_rate << "% (" << stats.modify_hits << "/" << stats.modify_attempts
            << "), max resting orders " << stats.max_resting << ", out-of-window rests "
            << stats.out_of_window << " (" << oow_per_op << " per op)\n";

  // Guard against the generator silently losing its ability to exercise cancel and modify.
  if (stats.cancel_attempts >= kMinAttemptsForRateCheck) {
    EXPECT_GT(cancel_rate, kMinCancelHitRate) << "cancel hit rate fell below the minimum";
  }
  if (stats.modify_attempts >= kMinAttemptsForRateCheck) {
    EXPECT_GT(modify_rate, kMinModifyHitRate) << "modify hit rate fell below the minimum";
  }
  // Guard the window itself: the in-window scenario must never touch the fallback, and the
  // out-of-window scenarios must exercise it for real.
  if (fc.forbid_out_of_window) {
    EXPECT_EQ(stats.out_of_window, 0u) << "an in-window scenario used the slow path";
  } else {
    EXPECT_GT(oow_per_op, fc.min_out_of_window_per_op)
        << "the fallback path was not exercised enough";
  }
}

// Default generator band 90..110 and the default window: every price is in the window.
TEST(Fuzz, NaiveMatcherAndOrderBookAgree) {
  run_config({"in-window", GenConfig{}, 0, OrderBook::kDefaultWindowTicks, 1.0, true, 0.0});
}

// Window 95..104 inside the band 90..110: about half of all prices are out of window, on both
// sides, so crossing across the window edge happens constantly.
TEST(Fuzz, WindowEdgeCrossing) {
  run_config({"window-edge", GenConfig{}, 95, 10, 0.25, false, 0.08});
}

// Zero-width window: every price goes through the fallback map.
TEST(Fuzz, EverythingThroughTheFallback) {
  run_config({"all-fallback", GenConfig{}, 95, 0, 0.25, false, 0.19});
}

// Wide price range 1..5000 with a 1000-tick window: about 80 percent of prices out of window.
TEST(Fuzz, WidePriceRange) {
  GenConfig g;
  g.price_min = 1;
  g.price_max = 5000;
  run_config({"wide-range", g, 2000, 1000, 0.5, false, 0.15});
}

// Normal band plus 5 percent of orders at prices anywhere in +-2^62: far outside any window,
// negative included, to stress the unsigned window test and the fallback ordering.
TEST(Fuzz, FarOutsideOrders) {
  GenConfig g;
  g.far_price_pct = 5;
  g.far_price_min = -(int64_t{1} << 62);
  g.far_price_max = int64_t{1} << 62;
  run_config({"far-outside", g, 0, OrderBook::kDefaultWindowTicks, 0.25, false, 0.007});
}

}  // namespace
