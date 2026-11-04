#include <gtest/gtest.h>

#include <string>
#include <type_traits>
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

using Implementations = ::testing::Types<NaiveMatcher, OrderBook>;

struct ImplementationName {
  template <class T>
  static std::string GetName(int) {
    if constexpr (std::is_same_v<T, NaiveMatcher>) {
      return "Naive";
    } else {
      return "OrderBook";
    }
  }
};

// Runs check_invariants() when the implementation has one (OrderBook), otherwise does nothing.
template <class M>
void check(const M& m) {
  if constexpr (requires { m.check_invariants(); }) {
    EXPECT_TRUE(m.check_invariants());
  }
}

// Empty m. The invariants are verified after every test for free.
template <class T>
class MatcherTest : public ::testing::Test {
 protected:
  void TearDown() override { check(m); }
  T m;
};
TYPED_TEST_SUITE(MatcherTest, Implementations, ImplementationName);

// The shared book from the spec:
//   asks 103: id3 x4 | 102: id2 x6 | 101: id1 x3, then id4 x2
//   bids 100: id11 x5, then id12 x3 | 99: id13 x10
template <class T>
class OriginalBookTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EXPECT_TRUE(m.submit_limit(3, Side::Sell, 103, 4).empty());
    EXPECT_TRUE(m.submit_limit(2, Side::Sell, 102, 6).empty());
    EXPECT_TRUE(m.submit_limit(1, Side::Sell, 101, 3).empty());
    EXPECT_TRUE(m.submit_limit(4, Side::Sell, 101, 2).empty());
    EXPECT_TRUE(m.submit_limit(11, Side::Buy, 100, 5).empty());
    EXPECT_TRUE(m.submit_limit(12, Side::Buy, 100, 3).empty());
    EXPECT_TRUE(m.submit_limit(13, Side::Buy, 99, 10).empty());
    check(m);
  }
  void TearDown() override { check(m); }
  T m;
};
TYPED_TEST_SUITE(OriginalBookTest, Implementations, ImplementationName);

TYPED_TEST(OriginalBookTest, InitialBestPrices) {
  auto& m = this->m;
  EXPECT_EQ(m.best_bid(), 100);
  EXPECT_EQ(m.best_ask(), 101);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 5u);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 8u);
}

TYPED_TEST(OriginalBookTest, CrossingBuyMatchesPriceThenTimePriority) {
  auto& m = this->m;
  const Trades expected = {{1, 20, 101, 3}, {4, 20, 101, 2}, {2, 20, 102, 3}};
  EXPECT_EQ(m.submit_limit(20, Side::Buy, 102, 8), expected);

  EXPECT_EQ(m.best_ask(), 102);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 102), 3u);
  // Order 20 was fully filled and must not rest.
  EXPECT_EQ(m.best_bid(), 100);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 102), 0u);
}

TYPED_TEST(OriginalBookTest, NonCrossingBuyRestsBehindExistingBids) {
  auto& m = this->m;
  EXPECT_TRUE(m.submit_limit(20, Side::Buy, 100, 8).empty());
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 16u);

  // A sell sweeping the level must hit 11, then 12, then the new order 20.
  const Trades expected = {{11, 30, 100, 5}, {12, 30, 100, 3}, {20, 30, 100, 1}};
  EXPECT_EQ(m.submit_limit(30, Side::Sell, 100, 9), expected);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 7u);
}

TYPED_TEST(MatcherTest, MarketBuyDropsUnfilledRemainder) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 4);
  m.submit_limit(2, Side::Sell, 102, 2);

  const Trades expected = {{1, 50, 101, 4}, {2, 50, 102, 2}};
  EXPECT_EQ(m.submit_market(50, Side::Buy, 10), expected);

  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.best_ask().has_value());
}

TYPED_TEST(MatcherTest, PartialFillKeepsRestingOrderAtFront) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 5);
  m.submit_limit(2, Side::Sell, 101, 5);

  const Trades first = {{1, 3, 101, 2}};
  EXPECT_EQ(m.submit_limit(3, Side::Buy, 101, 2), first);
  check(m);

  // Order 1 has 3 left and must still be ahead of order 2.
  const Trades second = {{1, 4, 101, 3}, {2, 4, 101, 2}};
  EXPECT_EQ(m.submit_limit(4, Side::Buy, 101, 5), second);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

TYPED_TEST(MatcherTest, CancelOfFilledOrderReturnsFalseAndLeavesBookUnchanged) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Sell, 102, 4);
  m.submit_limit(3, Side::Buy, 101, 3);  // fully fills order 1

  EXPECT_FALSE(m.cancel(1));
  EXPECT_FALSE(m.cancel(999));  // unknown id
  EXPECT_EQ(m.best_ask(), 102);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 102), 4u);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 0u);
}

TYPED_TEST(MatcherTest, CancelRemovesRestingOrder) {
  auto& m = this->m;
  m.submit_limit(1, Side::Buy, 100, 5);
  EXPECT_TRUE(m.cancel(1));
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.cancel(1));
}

TYPED_TEST(MatcherTest, ModifySendsOrderToBackOfLevel) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Sell, 101, 3);

  EXPECT_TRUE(m.modify(1, 101, 3).empty());  // same price, but loses time priority

  const Trades expected = {{2, 9, 101, 3}, {1, 9, 101, 1}};
  EXPECT_EQ(m.submit_limit(9, Side::Buy, 101, 4), expected);
}

TYPED_TEST(MatcherTest, ModifyOfUnknownIdChangesNothing) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  EXPECT_TRUE(m.modify(42, 100, 5).empty());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
  EXPECT_FALSE(m.best_bid().has_value());
}

TYPED_TEST(MatcherTest, ModifyCanCrossAndTrade) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Buy, 100, 5);

  const Trades expected = {{1, 2, 101, 3}};
  EXPECT_EQ(m.modify(2, 101, 5), expected);
  EXPECT_EQ(m.best_bid(), 101);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 101), 2u);
}

TYPED_TEST(MatcherTest, ModifyToZeroQtyCancelsTheOrder) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  EXPECT_TRUE(m.modify(1, 101, 0).empty());
  EXPECT_FALSE(m.best_ask().has_value());
  EXPECT_FALSE(m.cancel(1));
}

TYPED_TEST(MatcherTest, MarketOrderOnEmptyBookDoesNothing) {
  auto& m = this->m;
  EXPECT_TRUE(m.submit_market(1, Side::Buy, 10).empty());
  EXPECT_TRUE(m.submit_market(2, Side::Sell, 10).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.best_ask().has_value());
}

TYPED_TEST(MatcherTest, ZeroQuantityIsRejected) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);

  EXPECT_TRUE(m.submit_limit(2, Side::Buy, 105, 0).empty());  // would cross if accepted
  EXPECT_TRUE(m.submit_market(3, Side::Buy, 0).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

TYPED_TEST(MatcherTest, DuplicateRestingIdIsRejected) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);

  EXPECT_TRUE(m.submit_limit(1, Side::Buy, 105, 5).empty());  // would cross if accepted
  EXPECT_TRUE(m.submit_market(1, Side::Buy, 5).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

TYPED_TEST(MatcherTest, FilledIdCanBeReused) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Buy, 101, 3);  // order 1 is gone

  EXPECT_TRUE(m.submit_limit(1, Side::Sell, 105, 2).empty());
  EXPECT_EQ(m.best_ask(), 105);
}

TYPED_TEST(MatcherTest, SweepsSeveralPriceLevelsInOneOrder) {
  auto& m = this->m;
  m.submit_limit(1, Side::Sell, 101, 2);
  m.submit_limit(2, Side::Sell, 102, 3);
  m.submit_limit(3, Side::Sell, 103, 4);
  m.submit_limit(4, Side::Sell, 104, 5);

  const Trades expected = {{1, 20, 101, 2}, {2, 20, 102, 3}, {3, 20, 103, 4}};
  EXPECT_EQ(m.submit_limit(20, Side::Buy, 103, 10), expected);

  // The 1 unit left over rests at the limit price; 104 is beyond it and untouched.
  EXPECT_EQ(m.best_bid(), 103);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 103), 1u);
  EXPECT_EQ(m.best_ask(), 104);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 104), 5u);
}

TYPED_TEST(MatcherTest, CancelInMiddleOfLevelKeepsRestOfQueueIntact) {
  auto& m = this->m;
  m.submit_limit(1, Side::Buy, 100, 2);
  m.submit_limit(2, Side::Buy, 100, 2);
  m.submit_limit(3, Side::Buy, 100, 2);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 6u);

  EXPECT_TRUE(m.cancel(2));
  check(m);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 4u);

  const Trades expected = {{1, 9, 100, 2}, {3, 9, 100, 2}};
  EXPECT_EQ(m.submit_limit(9, Side::Sell, 100, 4), expected);
  EXPECT_FALSE(m.best_bid().has_value());
}

TYPED_TEST(MatcherTest, CancellingLastOrderAtLevelRemovesTheLevel) {
  auto& m = this->m;
  m.submit_limit(1, Side::Buy, 100, 5);
  m.submit_limit(2, Side::Buy, 99, 5);
  m.submit_limit(3, Side::Sell, 105, 5);
  m.submit_limit(4, Side::Sell, 106, 5);

  EXPECT_TRUE(m.cancel(1));
  EXPECT_EQ(m.best_bid(), 99);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 0u);

  EXPECT_TRUE(m.cancel(3));
  EXPECT_EQ(m.best_ask(), 106);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 105), 0u);

  EXPECT_TRUE(m.cancel(2));
  EXPECT_TRUE(m.cancel(4));
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.best_ask().has_value());
}

TYPED_TEST(MatcherTest, InvariantsHoldAfterEveryOperationInScriptedSequence) {
  auto& m = this->m;
  // check() after each step is a no-op for implementations without invariants.

  m.submit_limit(1, Side::Buy, 100, 10);
  check(m);
  m.submit_limit(2, Side::Buy, 100, 5);
  check(m);
  m.submit_limit(3, Side::Buy, 98, 7);
  check(m);
  m.submit_limit(4, Side::Sell, 105, 4);
  check(m);
  m.submit_limit(5, Side::Sell, 105, 6);
  check(m);
  m.submit_limit(6, Side::Sell, 107, 3);
  check(m);
  EXPECT_EQ(m.best_bid(), 100);
  EXPECT_EQ(m.best_ask(), 105);

  m.submit_limit(7, Side::Sell, 100, 12);  // eats order 1 fully and part of order 2
  check(m);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 3u);

  m.submit_limit(8, Side::Buy, 106, 10);  // eats both 105 orders exactly
  check(m);
  EXPECT_EQ(m.best_ask(), 107);

  EXPECT_TRUE(m.modify(2, 99, 4).empty());  // moves away from the market, no trades
  check(m);
  EXPECT_EQ(m.best_bid(), 99);

  EXPECT_TRUE(m.cancel(3));
  check(m);
  m.submit_market(9, Side::Sell, 2);
  check(m);
  m.submit_limit(10, Side::Buy, 0, 0);  // rejected
  check(m);
  m.submit_limit(2, Side::Buy, 90, 1);  // duplicate of a resting id: rejected
  check(m);
  EXPECT_FALSE(m.cancel(1));  // already filled
  check(m);
  m.submit_market(11, Side::Buy, 100);  // sweeps everything left on the ask side
  check(m);
  EXPECT_FALSE(m.best_ask().has_value());
  m.submit_limit(12, Side::Sell, 90, 50);  // sweeps remaining bids, then rests
  check(m);
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_EQ(m.best_ask(), 90);
}

using Levels = std::vector<std::pair<int64_t, uint32_t>>;

TEST(OrderBookDepth, ListsLevelsBestPriceFirst) {
  OrderBook book;
  EXPECT_TRUE(book.depth(Side::Buy).empty());
  EXPECT_TRUE(book.depth(Side::Sell).empty());

  book.submit_limit(3, Side::Sell, 103, 4);
  book.submit_limit(2, Side::Sell, 102, 6);
  book.submit_limit(1, Side::Sell, 101, 3);
  book.submit_limit(4, Side::Sell, 101, 2);
  book.submit_limit(11, Side::Buy, 100, 5);
  book.submit_limit(12, Side::Buy, 100, 3);
  book.submit_limit(13, Side::Buy, 99, 10);

  EXPECT_EQ(book.depth(Side::Sell), (Levels{{101, 5}, {102, 6}, {103, 4}}));
  EXPECT_EQ(book.depth(Side::Buy), (Levels{{100, 8}, {99, 10}}));

  // Fully fills level 101 and takes 3 from level 102.
  book.submit_limit(20, Side::Buy, 102, 8);
  EXPECT_EQ(book.depth(Side::Sell), (Levels{{102, 3}, {103, 4}}));
  EXPECT_EQ(book.depth(Side::Buy), (Levels{{100, 8}, {99, 10}}));

  EXPECT_TRUE(book.cancel(13));  // last order at 99, so the level disappears
  EXPECT_EQ(book.depth(Side::Buy), (Levels{{100, 8}}));
  EXPECT_TRUE(book.check_invariants());
}

TEST(OrderBookDepth, IsRestingTracksLiveOrders) {
  OrderBook book;
  book.submit_limit(1, Side::Sell, 101, 3);
  EXPECT_TRUE(book.is_resting(1));
  EXPECT_FALSE(book.is_resting(2));
  book.submit_limit(2, Side::Buy, 101, 3);  // fills order 1 completely
  EXPECT_FALSE(book.is_resting(1));
  EXPECT_FALSE(book.is_resting(2));
}

// A buffer pre-filled with junk, to prove OrderBook clears it rather than appending.
Trades dirty_buffer() { return Trades(3, Trade{99, 99, 99, 99}); }

TEST(OrderBookBuffer, EveryCallClearsTheBufferFirst) {
  OrderBook book;

  Trades out = dirty_buffer();
  book.submit_limit(1, Side::Sell, 101, 3, out);  // rests, no trades
  EXPECT_TRUE(out.empty());

  out = dirty_buffer();
  book.submit_limit(2, Side::Buy, 101, 2, out);
  EXPECT_EQ(out, (Trades{{1, 2, 101, 2}}));

  out = dirty_buffer();
  book.submit_limit(3, Side::Buy, 105, 0, out);  // qty 0 rejected
  EXPECT_TRUE(out.empty());

  out = dirty_buffer();
  book.submit_limit(1, Side::Buy, 105, 5, out);  // duplicate of a resting id rejected
  EXPECT_TRUE(out.empty());

  out = dirty_buffer();
  book.submit_market(4, Side::Buy, 10, out);
  EXPECT_EQ(out, (Trades{{1, 4, 101, 1}}));

  out = dirty_buffer();
  book.submit_market(5, Side::Buy, 0, out);  // qty 0 rejected
  EXPECT_TRUE(out.empty());

  out = dirty_buffer();
  book.modify(77, 100, 5, out);  // unknown id
  EXPECT_TRUE(out.empty());

  book.submit_limit(6, Side::Sell, 102, 4, out);
  book.submit_limit(7, Side::Buy, 100, 4, out);
  out = dirty_buffer();
  book.modify(7, 102, 4, out);  // crosses: fills against order 6
  EXPECT_EQ(out, (Trades{{6, 7, 102, 4}}));

  out = dirty_buffer();
  book.modify(6, 103, 0, out);  // qty 0 cancels; nothing to report
  EXPECT_TRUE(out.empty());
}

TEST(OrderBookBuffer, ReusedBufferKeepsItsCapacity) {
  OrderBook book;
  for (uint64_t i = 0; i < 20; ++i) book.submit_limit(i + 1, Side::Sell, 101, 1);

  Trades out;
  book.submit_market(100, Side::Buy, 20, out);  // 20 trades: the buffer grows once
  ASSERT_EQ(out.size(), 20u);
  const auto capacity = out.capacity();
  const auto* data = out.data();

  book.submit_limit(200, Side::Sell, 101, 3, out);
  book.submit_limit(201, Side::Buy, 101, 3, out);  // 1 trade into the same buffer
  EXPECT_EQ(out.size(), 1u);
  EXPECT_EQ(out.capacity(), capacity);  // clear() kept the allocation
  EXPECT_EQ(out.data(), data);          // and it was not reallocated
}

TEST(OrderBookBuffer, WrappersAgreeWithTheBufferApi) {
  // The same script through the vector-returning wrappers and through the buffer API.
  OrderBook wrapped, buffered;
  Trades out;

  EXPECT_TRUE(wrapped.submit_limit(1, Side::Sell, 101, 5).empty());
  buffered.submit_limit(1, Side::Sell, 101, 5, out);
  EXPECT_TRUE(out.empty());

  EXPECT_EQ(wrapped.submit_limit(2, Side::Buy, 101, 2), (Trades{{1, 2, 101, 2}}));
  buffered.submit_limit(2, Side::Buy, 101, 2, out);
  EXPECT_EQ(out, (Trades{{1, 2, 101, 2}}));

  EXPECT_EQ(wrapped.submit_market(3, Side::Buy, 9), (Trades{{1, 3, 101, 3}}));
  buffered.submit_market(3, Side::Buy, 9, out);
  EXPECT_EQ(out, (Trades{{1, 3, 101, 3}}));

  EXPECT_TRUE(wrapped.submit_limit(4, Side::Buy, 100, 4).empty());
  buffered.submit_limit(4, Side::Buy, 100, 4, out);
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(wrapped.modify(4, 100, 6).empty());
  buffered.modify(4, 100, 6, out);
  EXPECT_TRUE(out.empty());

  EXPECT_EQ(wrapped.best_bid(), buffered.best_bid());
  EXPECT_EQ(wrapped.best_ask(), buffered.best_ask());
  EXPECT_EQ(wrapped.total_qty_at(Side::Buy, 100), buffered.total_qty_at(Side::Buy, 100));
}

}  // namespace
