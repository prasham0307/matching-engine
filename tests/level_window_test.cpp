// OrderBook behavior around the edges of its price window. Every scenario runs on an OrderBook and
// a NaiveMatcher in lockstep, so each call is also checked against the oracle.
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "lob/naive_matcher.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

namespace {

using lob::NaiveMatcher;
using lob::OrderBook;
using lob::Side;
using lob::Trade;
using Trades = std::vector<Trade>;
using Levels = std::vector<std::pair<int64_t, uint32_t>>;

// Window [100, 108): 100..107 in window, everything else is the slow path.
constexpr int64_t kBase = 100;
constexpr std::size_t kWindow = 8;

class Lockstep {
 public:
  Lockstep(int64_t base = kBase, std::size_t window = kWindow) : book(64, base, window) {}

  Trades limit(uint64_t id, Side side, int64_t price, uint32_t qty) {
    const Trades expected = naive.submit_limit(id, side, price, qty);
    EXPECT_EQ(book.submit_limit(id, side, price, qty), expected);
    verify();
    return expected;
  }
  Trades market(uint64_t id, Side side, uint32_t qty) {
    const Trades expected = naive.submit_market(id, side, qty);
    EXPECT_EQ(book.submit_market(id, side, qty), expected);
    verify();
    return expected;
  }
  bool cancel(uint64_t id) {
    const bool expected = naive.cancel(id);
    EXPECT_EQ(book.cancel(id), expected);
    verify();
    return expected;
  }
  Trades modify(uint64_t id, int64_t price, uint32_t qty) {
    const Trades expected = naive.modify(id, price, qty);
    EXPECT_EQ(book.modify(id, price, qty), expected);
    verify();
    return expected;
  }

  void verify() {
    EXPECT_EQ(book.best_bid(), naive.best_bid());
    EXPECT_EQ(book.best_ask(), naive.best_ask());
    EXPECT_TRUE(book.check_invariants());
    // total_qty_at must agree on every window tick, the ticks just outside it, and the depth.
    for (int64_t p = kBase - 3; p < kBase + static_cast<int64_t>(kWindow) + 3; ++p) {
      for (const Side s : {Side::Buy, Side::Sell}) {
        EXPECT_EQ(book.total_qty_at(s, p), naive.total_qty_at(s, p)) << "price " << p;
      }
    }
    for (const Side s : {Side::Buy, Side::Sell}) {
      for (const auto& [price, qty] : book.depth(s)) {
        EXPECT_EQ(naive.total_qty_at(s, price), qty) << "depth price " << price;
      }
    }
  }

  OrderBook book;
  NaiveMatcher naive;
};

TEST(LevelWindow, BuySweepCrossesTheUpperWindowEdgeInPriceOrder) {
  Lockstep t;
  t.limit(1, Side::Sell, 106, 2);  // in window
  t.limit(2, Side::Sell, 107, 2);  // last in-window tick
  t.limit(3, Side::Sell, 108, 2);  // first tick past the window
  t.limit(4, Side::Sell, 120, 2);  // far outside
  EXPECT_EQ(t.book.out_of_window_orders(), 2u);

  const Trades trades = t.limit(10, Side::Buy, 125, 7);
  const Trades expected = {{1, 10, 106, 2}, {2, 10, 107, 2}, {3, 10, 108, 2}, {4, 10, 120, 1}};
  EXPECT_EQ(trades, expected);
  EXPECT_EQ(t.book.best_ask(), 120);
}

TEST(LevelWindow, SellSweepCrossesTheLowerWindowEdgeInPriceOrder) {
  Lockstep t;
  t.limit(1, Side::Buy, 101, 2);
  t.limit(2, Side::Buy, 100, 2);  // first in-window tick
  t.limit(3, Side::Buy, 99, 2);   // first tick below the window
  t.limit(4, Side::Buy, 80, 2);   // far outside

  const Trades trades = t.limit(10, Side::Sell, 70, 7);
  const Trades expected = {{1, 10, 101, 2}, {2, 10, 100, 2}, {3, 10, 99, 2}, {4, 10, 80, 1}};
  EXPECT_EQ(trades, expected);
  EXPECT_EQ(t.book.best_bid(), 80);
}

TEST(LevelWindow, BestCursorMovesToTheNextLevelWhenTheBestEmpties) {
  Lockstep t;
  t.limit(1, Side::Buy, 106, 3);
  t.limit(2, Side::Buy, 104, 3);
  t.limit(3, Side::Buy, 101, 3);
  EXPECT_EQ(t.book.best_bid(), 106);

  t.limit(10, Side::Sell, 106, 3);  // empties the best bid exactly
  EXPECT_EQ(t.book.best_bid(), 104);
  t.market(11, Side::Sell, 3);      // and the next
  EXPECT_EQ(t.book.best_bid(), 101);
  t.limit(12, Side::Sell, 101, 1);  // partial: the level stays, the cursor must not move
  EXPECT_EQ(t.book.best_bid(), 101);
  EXPECT_EQ(t.book.total_qty_at(Side::Buy, 101), 2u);
  t.market(13, Side::Sell, 5);
  EXPECT_EQ(t.book.best_bid(), std::nullopt);
}

TEST(LevelWindow, BestAskCursorMovesUpAsLevelsEmpty) {
  Lockstep t;
  t.limit(1, Side::Sell, 101, 3);
  t.limit(2, Side::Sell, 105, 3);
  t.limit(3, Side::Sell, 107, 3);
  EXPECT_EQ(t.book.best_ask(), 101);
  t.market(10, Side::Buy, 3);
  EXPECT_EQ(t.book.best_ask(), 105);
  t.market(11, Side::Buy, 3);
  EXPECT_EQ(t.book.best_ask(), 107);
  t.market(12, Side::Buy, 3);
  EXPECT_EQ(t.book.best_ask(), std::nullopt);
}

TEST(LevelWindow, CancellingTheLastOrderAtALevelRemovesIt) {
  Lockstep t;
  t.limit(1, Side::Buy, 105, 4);   // in window, best
  t.limit(2, Side::Buy, 105, 6);   // same level, behind
  t.limit(3, Side::Buy, 102, 1);
  t.limit(4, Side::Buy, 130, 1);   // out of window, and the best bid of all
  EXPECT_EQ(t.book.best_bid(), 130);

  EXPECT_TRUE(t.cancel(4));        // last (only) order at an out-of-window level
  EXPECT_EQ(t.book.best_bid(), 105);
  EXPECT_TRUE(t.cancel(1));        // level survives: order 2 is still there
  EXPECT_EQ(t.book.best_bid(), 105);
  EXPECT_EQ(t.book.total_qty_at(Side::Buy, 105), 6u);
  EXPECT_TRUE(t.cancel(2));        // last order at the best in-window level
  EXPECT_EQ(t.book.best_bid(), 102);
  EXPECT_EQ(t.book.depth(Side::Buy), (Levels{{102, 1}}));
  EXPECT_TRUE(t.cancel(3));
  EXPECT_EQ(t.book.best_bid(), std::nullopt);
  EXPECT_FALSE(t.cancel(3));
}

TEST(LevelWindow, OrdersExactlyAtTheWindowBoundaries) {
  Lockstep t;
  // Bids on the four ticks around the lower edge and the upper edge of [100, 108).
  t.limit(1, Side::Buy, 99, 1);    // just below: slow path
  t.limit(2, Side::Buy, 100, 1);   // first in-window tick
  t.limit(3, Side::Buy, 107, 1);   // last in-window tick
  t.limit(4, Side::Buy, 108, 1);   // just above: slow path
  EXPECT_EQ(t.book.out_of_window_orders(), 2u);
  EXPECT_EQ(t.book.depth(Side::Buy), (Levels{{108, 1}, {107, 1}, {100, 1}, {99, 1}}));
  EXPECT_EQ(t.book.best_bid(), 108);

  // Asks above everything, so nothing crosses yet.
  t.limit(5, Side::Sell, 120, 1);
  t.limit(6, Side::Sell, 109, 1);  // fallback
  EXPECT_EQ(t.book.depth(Side::Sell), (Levels{{109, 1}, {120, 1}}));

  // Take the bids down one at a time with market sells: they must come off in price order,
  // across both window edges.
  const std::vector<int64_t> expected_prices = {108, 107, 100, 99};
  for (std::size_t i = 0; i < expected_prices.size(); ++i) {
    const Trades trades = t.market(20 + i, Side::Sell, 1);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, expected_prices[i]);
  }
  EXPECT_EQ(t.book.best_bid(), std::nullopt);
}

TEST(LevelWindow, OutOfWindowOrdersMatchInWindowOnes) {
  Lockstep t;
  t.limit(1, Side::Sell, 150, 5);  // rests out of window
  t.limit(2, Side::Sell, 104, 5);  // rests in window
  EXPECT_EQ(t.book.best_ask(), 104);

  // An out-of-window buyer that takes the in-window ask first, then the out-of-window one.
  const Trades trades = t.limit(10, Side::Buy, 160, 8);
  EXPECT_EQ(trades, (Trades{{2, 10, 104, 5}, {1, 10, 150, 3}}));
  EXPECT_EQ(t.book.total_qty_at(Side::Sell, 150), 2u);

  // An in-window sell resting against an out-of-window bid: the bid is the better price.
  t.limit(11, Side::Buy, 110, 4);  // out of window (above)
  const Trades back = t.limit(12, Side::Sell, 103, 6);
  EXPECT_EQ(back, (Trades{{11, 12, 110, 4}}));  // trades at the resting price, 110
  EXPECT_EQ(t.book.best_bid(), std::nullopt);
  EXPECT_EQ(t.book.best_ask(), 103);
}

TEST(LevelWindow, ModifyMovesAnOrderAcrossTheWindowEdgeAndBack) {
  Lockstep t;
  t.limit(1, Side::Buy, 105, 3);
  EXPECT_EQ(t.book.out_of_window_orders(), 0u);
  t.modify(1, 140, 3);  // in window -> out of window
  EXPECT_EQ(t.book.out_of_window_orders(), 1u);
  EXPECT_EQ(t.book.best_bid(), 140);
  EXPECT_EQ(t.book.total_qty_at(Side::Buy, 105), 0u);
  t.modify(1, 103, 3);  // and back in
  EXPECT_EQ(t.book.best_bid(), 103);
  EXPECT_EQ(t.book.total_qty_at(Side::Buy, 140), 0u);
  EXPECT_EQ(t.book.depth(Side::Buy), (Levels{{103, 3}}));
}

TEST(LevelWindow, ExtremePricesRestAndMatch) {
  Lockstep t;
  t.limit(1, Side::Sell, INT64_MAX, 2);
  t.limit(2, Side::Buy, INT64_MIN, 2);
  EXPECT_EQ(t.book.best_ask(), INT64_MAX);
  EXPECT_EQ(t.book.best_bid(), INT64_MIN);
  t.limit(3, Side::Buy, INT64_MAX, 1);   // crosses the sell at INT64_MAX
  t.limit(4, Side::Sell, INT64_MIN, 1);  // crosses the buy at INT64_MIN
  EXPECT_EQ(t.book.total_qty_at(Side::Sell, INT64_MAX), 1u);
  EXPECT_EQ(t.book.total_qty_at(Side::Buy, INT64_MIN), 1u);
}

TEST(LevelWindow, DepthMergesWindowAndFallbackBestFirst) {
  Lockstep t;
  for (const auto& [id, price] : std::vector<std::pair<uint64_t, int64_t>>{
           {1, 90}, {2, 99}, {3, 100}, {4, 103}, {5, 107}, {6, 108}, {7, 115}}) {
    t.limit(id, Side::Buy, price, 1);
  }
  // No sells, so nothing matched: all seven bids rest.
  EXPECT_EQ(t.book.depth(Side::Buy),
            (Levels{{115, 1}, {108, 1}, {107, 1}, {103, 1}, {100, 1}, {99, 1}, {90, 1}}));
  EXPECT_EQ(t.book.out_of_window_orders(), 4u);  // 90, 99, 108, 115
}

TEST(LevelWindow, InWindowScenarioNeverTouchesTheFallback) {
  Lockstep t;
  for (uint64_t id = 1; id <= 40; ++id) {
    t.limit(id, id % 2 ? Side::Buy : Side::Sell, 100 + static_cast<int64_t>(id % 8), 1 + id % 3);
  }
  EXPECT_EQ(t.book.out_of_window_orders(), 0u);
}

TEST(LevelWindow, WindowAccessorsReportTheConfiguration) {
  OrderBook book(16, 90, 21);
  EXPECT_EQ(book.window_base(), 90);
  EXPECT_EQ(book.window_ticks(), 21u);
  OrderBook defaults;
  EXPECT_EQ(defaults.window_base(), 0);
  EXPECT_EQ(defaults.window_ticks(), OrderBook::kDefaultWindowTicks);
}

TEST(LevelWindow, MovedBookKeepsItsLevels) {
  OrderBook a(16, 100, 8);
  a.submit_limit(1, Side::Buy, 105, 2);
  a.submit_limit(2, Side::Buy, 150, 2);  // fallback
  OrderBook b(std::move(a));
  EXPECT_EQ(b.best_bid(), 150);
  EXPECT_EQ(b.depth(Side::Buy), (Levels{{150, 2}, {105, 2}}));
  EXPECT_TRUE(b.check_invariants());
  EXPECT_TRUE(b.cancel(2));
  EXPECT_EQ(b.best_bid(), 105);
}

}  // namespace
