#include "lob/naive_matcher.hpp"

#include <gtest/gtest.h>

#include <ostream>
#include <vector>

namespace lob {

// Test-only helpers so failures print readable trades.
bool operator==(const Trade& a, const Trade& b) {
  return a.makerId == b.makerId && a.takerId == b.takerId && a.price == b.price && a.qty == b.qty;
}
std::ostream& operator<<(std::ostream& os, const Trade& t) {
  return os << "(maker " << t.makerId << ", taker " << t.takerId << ", " << t.price << ", "
            << t.qty << ")";
}

}  // namespace lob

namespace {

using lob::NaiveMatcher;
using lob::Side;
using lob::Trade;
using Trades = std::vector<Trade>;

// The shared book from the spec:
//   asks 103: id3 x4 | 102: id2 x6 | 101: id1 x3, then id4 x2
//   bids 100: id11 x5, then id12 x3 | 99: id13 x10
class OriginalBook : public ::testing::Test {
 protected:
  void SetUp() override {
    EXPECT_TRUE(m.submit_limit(3, Side::Sell, 103, 4).empty());
    EXPECT_TRUE(m.submit_limit(2, Side::Sell, 102, 6).empty());
    EXPECT_TRUE(m.submit_limit(1, Side::Sell, 101, 3).empty());
    EXPECT_TRUE(m.submit_limit(4, Side::Sell, 101, 2).empty());
    EXPECT_TRUE(m.submit_limit(11, Side::Buy, 100, 5).empty());
    EXPECT_TRUE(m.submit_limit(12, Side::Buy, 100, 3).empty());
    EXPECT_TRUE(m.submit_limit(13, Side::Buy, 99, 10).empty());
  }
  NaiveMatcher m;
};

TEST_F(OriginalBook, InitialBestPrices) {
  EXPECT_EQ(m.best_bid(), 100);
  EXPECT_EQ(m.best_ask(), 101);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 5u);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 8u);
}

TEST_F(OriginalBook, CrossingBuyMatchesPriceThenTimePriority) {
  const Trades expected = {{1, 20, 101, 3}, {4, 20, 101, 2}, {2, 20, 102, 3}};
  EXPECT_EQ(m.submit_limit(20, Side::Buy, 102, 8), expected);

  EXPECT_EQ(m.best_ask(), 102);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 102), 3u);
  // Order 20 was fully filled and must not rest.
  EXPECT_EQ(m.best_bid(), 100);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 102), 0u);
}

TEST_F(OriginalBook, NonCrossingBuyRestsBehindExistingBids) {
  EXPECT_TRUE(m.submit_limit(20, Side::Buy, 100, 8).empty());
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 16u);

  // A sell sweeping the level must hit 11, then 12, then the new order 20.
  const Trades expected = {{11, 30, 100, 5}, {12, 30, 100, 3}, {20, 30, 100, 1}};
  EXPECT_EQ(m.submit_limit(30, Side::Sell, 100, 9), expected);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 100), 7u);
}

TEST(NaiveMatcher, MarketBuyDropsUnfilledRemainder) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 4);
  m.submit_limit(2, Side::Sell, 102, 2);

  const Trades expected = {{1, 50, 101, 4}, {2, 50, 102, 2}};
  EXPECT_EQ(m.submit_market(50, Side::Buy, 10), expected);

  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.best_ask().has_value());
}

TEST(NaiveMatcher, PartialFillKeepsRestingOrderAtFront) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 5);
  m.submit_limit(2, Side::Sell, 101, 5);

  const Trades first = {{1, 3, 101, 2}};
  EXPECT_EQ(m.submit_limit(3, Side::Buy, 101, 2), first);

  // Order 1 has 3 left and must still be ahead of order 2.
  const Trades second = {{1, 4, 101, 3}, {2, 4, 101, 2}};
  EXPECT_EQ(m.submit_limit(4, Side::Buy, 101, 5), second);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

TEST(NaiveMatcher, CancelOfFilledOrderReturnsFalseAndLeavesBookUnchanged) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Sell, 102, 4);
  m.submit_limit(3, Side::Buy, 101, 3);  // fully fills order 1

  EXPECT_FALSE(m.cancel(1));
  EXPECT_FALSE(m.cancel(999));  // unknown id
  EXPECT_EQ(m.best_ask(), 102);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 102), 4u);
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 0u);
}

TEST(NaiveMatcher, CancelRemovesRestingOrder) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Buy, 100, 5);
  EXPECT_TRUE(m.cancel(1));
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.cancel(1));
}

TEST(NaiveMatcher, ModifySendsOrderToBackOfLevel) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Sell, 101, 3);

  EXPECT_TRUE(m.modify(1, 101, 3).empty());  // same price, but loses time priority

  const Trades expected = {{2, 9, 101, 3}, {1, 9, 101, 1}};
  EXPECT_EQ(m.submit_limit(9, Side::Buy, 101, 4), expected);
}

TEST(NaiveMatcher, ModifyOfUnknownIdChangesNothing) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);
  EXPECT_TRUE(m.modify(42, 100, 5).empty());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
  EXPECT_FALSE(m.best_bid().has_value());
}

TEST(NaiveMatcher, ModifyCanCrossAndTrade) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);
  m.submit_limit(2, Side::Buy, 100, 5);

  const Trades expected = {{1, 2, 101, 3}};
  EXPECT_EQ(m.modify(2, 101, 5), expected);
  EXPECT_EQ(m.best_bid(), 101);
  EXPECT_EQ(m.total_qty_at(Side::Buy, 101), 2u);
}

TEST(NaiveMatcher, MarketOrderOnEmptyBookDoesNothing) {
  NaiveMatcher m;
  EXPECT_TRUE(m.submit_market(1, Side::Buy, 10).empty());
  EXPECT_TRUE(m.submit_market(2, Side::Sell, 10).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_FALSE(m.best_ask().has_value());
}

TEST(NaiveMatcher, ZeroQuantityIsRejected) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);

  EXPECT_TRUE(m.submit_limit(2, Side::Buy, 105, 0).empty());  // would cross if accepted
  EXPECT_TRUE(m.submit_market(3, Side::Buy, 0).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

TEST(NaiveMatcher, DuplicateRestingIdIsRejected) {
  NaiveMatcher m;
  m.submit_limit(1, Side::Sell, 101, 3);

  EXPECT_TRUE(m.submit_limit(1, Side::Buy, 105, 5).empty());  // would cross if accepted
  EXPECT_TRUE(m.submit_market(1, Side::Buy, 5).empty());
  EXPECT_FALSE(m.best_bid().has_value());
  EXPECT_EQ(m.total_qty_at(Side::Sell, 101), 3u);
}

}  // namespace
