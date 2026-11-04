#include "lob/matching_engine.hpp"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "lob/naive_matcher.hpp"
#include "lob/order_generator.hpp"
#include "test_util.hpp"

namespace {

using lob::EngineConfig;
using lob::MatchingEngine;
using lob::NaiveMatcher;
using lob::Op;
using lob::OpKind;
using lob::OrderGenerator;
using lob::Side;
using lob::Trade;
using Trades = std::vector<Trade>;

constexpr uint32_t kA = 7;
constexpr uint32_t kB = 1000;
constexpr uint32_t kC = 4000000000u;  // a large symbol value, to exercise the lookup

// Wrappers that return the trades, for readable tests.
Trades limit(MatchingEngine& e, uint32_t sym, uint64_t id, Side side, int64_t price, uint32_t qty) {
  Trades out;
  e.submit_limit(sym, id, side, price, qty, out);
  return out;
}
Trades market(MatchingEngine& e, uint32_t sym, uint64_t id, Side side, uint32_t qty) {
  Trades out;
  e.submit_market(sym, id, side, qty, out);
  return out;
}

TEST(MatchingEngine, SymbolsNeverTradeAgainstEachOther) {
  MatchingEngine e;
  EXPECT_TRUE(limit(e, kA, 1, Side::Buy, 100, 5).empty());
  EXPECT_TRUE(limit(e, kB, 2, Side::Sell, 100, 5).empty());  // would cross if it were the same book

  EXPECT_EQ(e.best_bid(kA), 100);
  EXPECT_EQ(e.best_ask(kA), std::nullopt);
  EXPECT_EQ(e.best_bid(kB), std::nullopt);
  EXPECT_EQ(e.best_ask(kB), 100);
  EXPECT_EQ(e.resting_orders(), 2u);

  // A market order on one symbol takes only that symbol's liquidity.
  EXPECT_EQ(market(e, kA, 3, Side::Sell, 5), (Trades{{1, 3, 100, 5}}));
  EXPECT_EQ(e.best_ask(kB), 100);
  EXPECT_EQ(e.total_qty_at(kB, Side::Sell, 100), 5u);
  EXPECT_TRUE(e.check_invariants());
}

TEST(MatchingEngine, EachSymbolMatchesWithPriceTimePriority) {
  MatchingEngine e;
  limit(e, kA, 1, Side::Sell, 101, 3);
  limit(e, kB, 2, Side::Sell, 101, 3);
  limit(e, kA, 3, Side::Sell, 101, 2);
  EXPECT_EQ(limit(e, kA, 10, Side::Buy, 101, 4), (Trades{{1, 10, 101, 3}, {3, 10, 101, 1}}));
  EXPECT_EQ(e.total_qty_at(kB, Side::Sell, 101), 3u);
}

// Documented rule: ids are global. An id resting on any symbol is taken everywhere.
TEST(MatchingEngine, AnIdRestingOnOneSymbolIsRejectedOnAnother) {
  MatchingEngine e;
  limit(e, kA, 7, Side::Buy, 100, 5);
  EXPECT_EQ(e.symbol_of(7), kA);

  EXPECT_TRUE(limit(e, kB, 7, Side::Sell, 99, 5).empty());  // would cross kA's bid if accepted
  EXPECT_EQ(e.symbol_of(7), kA);                            // unchanged
  EXPECT_EQ(e.best_ask(kB), std::nullopt);                  // nothing rested on kB
  EXPECT_FALSE(e.has_symbol(kB));                           // and rejection created no book
  EXPECT_TRUE(market(e, kA, 7, Side::Sell, 1).empty());     // market orders follow the same rule
  EXPECT_EQ(e.total_qty_at(kA, Side::Buy, 100), 5u);
  EXPECT_EQ(e.resting_orders(), 1u);
}

TEST(MatchingEngine, AnIdIsFreeAgainOnceItsOrderIsGone) {
  MatchingEngine e;
  limit(e, kA, 7, Side::Buy, 100, 5);
  EXPECT_TRUE(e.cancel(7));
  EXPECT_EQ(e.symbol_of(7), std::nullopt);
  EXPECT_TRUE(limit(e, kB, 7, Side::Sell, 100, 3).empty());  // reusable on another symbol
  EXPECT_EQ(e.symbol_of(7), kB);

  // Gone by being filled works the same way.
  EXPECT_EQ(market(e, kB, 8, Side::Buy, 3), (Trades{{7, 8, 100, 3}}));
  EXPECT_EQ(e.symbol_of(7), std::nullopt);
  EXPECT_EQ(e.resting_orders(), 0u);
  EXPECT_TRUE(limit(e, kC, 7, Side::Buy, 50, 1).empty());
  EXPECT_EQ(e.symbol_of(7), kC);
  EXPECT_TRUE(e.check_invariants());
}

TEST(MatchingEngine, CancelOfAnUnknownSymbolOrIdReturnsFalse) {
  MatchingEngine e;
  limit(e, kA, 1, Side::Buy, 100, 5);

  EXPECT_FALSE(e.cancel(kC, 1));    // symbol never seen
  EXPECT_FALSE(e.cancel(kB, 1));    // id exists, but on a different symbol
  EXPECT_FALSE(e.cancel(kA, 99));   // known symbol, unknown id
  EXPECT_FALSE(e.cancel(99));       // unknown id
  EXPECT_EQ(e.total_qty_at(kA, Side::Buy, 100), 5u);  // none of that touched the order
  EXPECT_FALSE(e.has_symbol(kC));                     // and none created a book

  EXPECT_TRUE(e.cancel(kA, 1));
  EXPECT_FALSE(e.cancel(kA, 1));    // already gone
  EXPECT_EQ(e.resting_orders(), 0u);
}

TEST(MatchingEngine, CancelByIdFindsTheRightBook) {
  MatchingEngine e;
  limit(e, kA, 1, Side::Buy, 100, 5);
  limit(e, kB, 2, Side::Buy, 100, 6);
  limit(e, kC, 3, Side::Buy, 100, 7);
  EXPECT_TRUE(e.cancel(2));
  EXPECT_EQ(e.total_qty_at(kA, Side::Buy, 100), 5u);
  EXPECT_EQ(e.total_qty_at(kB, Side::Buy, 100), 0u);
  EXPECT_EQ(e.total_qty_at(kC, Side::Buy, 100), 7u);
}

TEST(MatchingEngine, ModifyResolvesTheSymbolAndCanTrade) {
  MatchingEngine e;
  limit(e, kA, 1, Side::Sell, 101, 3);
  limit(e, kB, 2, Side::Sell, 101, 3);
  limit(e, kB, 3, Side::Buy, 100, 5);

  Trades out;
  EXPECT_TRUE(e.modify(3, 101, 5, out));  // crosses kB's ask only
  EXPECT_EQ(out, (Trades{{2, 3, 101, 3}}));
  EXPECT_EQ(e.total_qty_at(kA, Side::Sell, 101), 3u);  // kA untouched
  EXPECT_EQ(e.symbol_of(3), kB);                       // 2 left resting on kB

  EXPECT_FALSE(e.modify(99, 100, 5, out));  // unknown id changes nothing
  EXPECT_TRUE(out.empty());

  EXPECT_TRUE(e.modify(3, 100, 0, out));    // new qty 0 cancels
  EXPECT_EQ(e.symbol_of(3), std::nullopt);
  EXPECT_TRUE(e.check_invariants());
}

TEST(MatchingEngine, MarketOrderOnAnUnknownSymbolDoesNothing) {
  MatchingEngine e;
  EXPECT_TRUE(market(e, kC, 1, Side::Buy, 10).empty());
  EXPECT_FALSE(e.has_symbol(kC));
  EXPECT_TRUE(e.symbols().empty());
}

TEST(MatchingEngine, ZeroQuantityIsRejectedWithoutCreatingABook) {
  MatchingEngine e;
  EXPECT_TRUE(limit(e, kA, 1, Side::Buy, 100, 0).empty());
  EXPECT_FALSE(e.has_symbol(kA));
  EXPECT_EQ(e.resting_orders(), 0u);
}

TEST(MatchingEngine, SymbolsAreListedInTheOrderTheyWereFirstSeen) {
  MatchingEngine e;
  limit(e, kC, 1, Side::Buy, 100, 1);
  limit(e, kA, 2, Side::Buy, 100, 1);
  limit(e, kC, 3, Side::Buy, 99, 1);
  limit(e, kB, 4, Side::Buy, 100, 1);
  EXPECT_EQ(e.symbols(), (std::vector<uint32_t>{kC, kA, kB}));
  EXPECT_NE(e.book(kA), nullptr);
  EXPECT_EQ(e.book(12345), nullptr);
}

TEST(MatchingEngine, ManySymbolsGrowTheTablesTransparently) {
  EngineConfig config;
  config.expected_symbols = 1;
  config.expected_resting_orders = 1;
  MatchingEngine e(config);
  for (uint32_t s = 0; s < 300; ++s) {
    limit(e, s * 977 + 1, s + 1, Side::Buy, 100, 1);
  }
  EXPECT_EQ(e.symbols().size(), 300u);
  EXPECT_EQ(e.resting_orders(), 300u);
  EXPECT_GT(e.id_table_resizes(), 0u);
  for (uint32_t s = 0; s < 300; ++s) EXPECT_EQ(e.symbol_of(s + 1), s * 977 + 1);
  EXPECT_TRUE(e.check_invariants());
  EXPECT_TRUE(e.cancel(150));
  EXPECT_TRUE(e.check_invariants());
}

TEST(MatchingEngine, PresizedEngineDoesNotGrowItsIdTable) {
  EngineConfig config;
  config.expected_resting_orders = 2000;
  MatchingEngine e(config);
  for (uint64_t id = 1; id <= 2000; ++id) limit(e, kA, id, Side::Buy, 100, 1);
  EXPECT_EQ(e.id_table_resizes(), 0u);
}

// Three symbols through the engine, each checked against its own NaiveMatcher. The oracle applies
// the same rules the engine documents: ids are global, cancel and modify find the order wherever
// it rests. Recent ids are re-submitted on random symbols now and then, so duplicates (including
// across symbols) and id reuse after a fill are exercised.
struct Counters {
  uint64_t trades = 0;
  uint64_t cancel_hits = 0, cancels = 0;
  uint64_t cross_symbol_duplicates = 0;
  uint64_t id_reuses = 0;
  std::map<uint32_t, uint64_t> trades_per_symbol;
};

void run_seed(uint64_t seed, uint64_t n, Counters& c) {
  const std::vector<uint32_t> syms = {kA, kB, kC};
  std::map<uint32_t, NaiveMatcher> oracle;
  for (uint32_t s : syms) oracle[s];

  lob::GenConfig cfg;
  cfg.seed = seed;
  OrderGenerator gen(cfg);
  std::mt19937_64 pick(seed * 7919 + 1);
  MatchingEngine engine;
  Trades got;
  uint64_t max_issued = 0;

  const auto owner_of = [&](uint64_t id) -> std::optional<uint32_t> {
    for (uint32_t s : syms) {
      if (oracle[s].is_resting(id)) return s;
    }
    return std::nullopt;
  };

  for (uint64_t i = 0; i < n; ++i) {
    Op op = gen.next();
    if (op.kind == OpKind::Limit || op.kind == OpKind::Market) max_issued = std::max(max_issued, op.id);
    if (i % 40 == 13 && max_issued > 0) {  // re-submit a recent id on a random symbol
      op.kind = OpKind::Limit;
      op.id = 1 + pick() % max_issued;
      op.side = pick() % 2 ? Side::Buy : Side::Sell;
      op.price = 90 + static_cast<int64_t>(pick() % 21);
      op.qty = 1 + static_cast<uint32_t>(pick() % 20);
    }
    const uint32_t sym = syms[pick() % syms.size()];
    Trades expected;
    const std::string where = "seed " + std::to_string(seed) + " op " + std::to_string(i);

    switch (op.kind) {
      case OpKind::Limit:
      case OpKind::Market: {
        const auto owner = owner_of(op.id);
        if (owner) {
          if (*owner != sym) ++c.cross_symbol_duplicates;
        } else {
          expected = op.kind == OpKind::Limit ? oracle[sym].submit_limit(op.id, op.side, op.price, op.qty)
                                              : oracle[sym].submit_market(op.id, op.side, op.qty);
        }
        if (op.kind == OpKind::Limit) {
          engine.submit_limit(sym, op.id, op.side, op.price, op.qty, got);
        } else {
          engine.submit_market(sym, op.id, op.side, op.qty, got);
        }
        ASSERT_EQ(got, expected) << where;
        break;
      }
      case OpKind::Cancel: {
        const auto owner = owner_of(op.id);
        bool expected_hit;
        bool got_hit;
        if (pick() % 2 == 0) {
          expected_hit = owner.has_value() && oracle[*owner].cancel(op.id);
          got_hit = engine.cancel(op.id);
        } else {
          expected_hit = owner.has_value() && *owner == sym && oracle[sym].cancel(op.id);
          got_hit = engine.cancel(sym, op.id);
        }
        ASSERT_EQ(got_hit, expected_hit) << where;
        ++c.cancels;
        if (got_hit) ++c.cancel_hits;
        break;
      }
      case OpKind::Modify: {
        const auto owner = owner_of(op.id);
        if (owner) expected = oracle[*owner].modify(op.id, op.price, op.qty);
        const bool got_found = engine.modify(op.id, op.price, op.qty, got);
        ASSERT_EQ(got_found, owner.has_value()) << where;
        ASSERT_EQ(got, expected) << where;
        break;
      }
    }

    c.trades += got.size();
    if (!got.empty() && op.kind != OpKind::Cancel) {
      const uint32_t trade_symbol = (op.kind == OpKind::Modify && engine.symbol_of(op.id))
                                        ? *engine.symbol_of(op.id)
                                        : sym;
      c.trades_per_symbol[trade_symbol] += got.size();
    }

    std::size_t resting = 0;
    for (uint32_t s : syms) {
      ASSERT_EQ(engine.best_bid(s), oracle[s].best_bid()) << where;
      ASSERT_EQ(engine.best_ask(s), oracle[s].best_ask()) << where;
      resting += oracle[s].resting_count();
    }
    ASSERT_EQ(engine.resting_orders(), resting) << where;

    if ((i + 1) % 1000 == 0 || i + 1 == n) {
      for (uint32_t s : syms) {
        for (int64_t p = 90; p <= 110; ++p) {
          for (Side side : {Side::Buy, Side::Sell}) {
            ASSERT_EQ(engine.total_qty_at(s, side, p), oracle[s].total_qty_at(side, p))
                << where << " symbol " << s << " price " << p;
          }
        }
      }
      ASSERT_TRUE(engine.check_invariants()) << where;
    }
  }
}

TEST(MatchingEngine, RandomizedThreeSymbolsAgainstPerSymbolOracles) {
#ifdef NDEBUG
  constexpr uint64_t kOps = 150000;
#else
  constexpr uint64_t kOps = 25000;
#endif
  Counters c;
  for (const uint64_t seed : {1u, 2u, 3u}) {
    run_seed(seed, kOps, c);
    if (HasFatalFailure()) return;
  }
  std::cout << "[engine fuzz] 3 seeds x " << kOps << " ops, " << c.trades << " trades, cancel hit rate "
            << 100.0 * static_cast<double>(c.cancel_hits) / static_cast<double>(c.cancels)
            << "%, cross-symbol duplicate rejections " << c.cross_symbol_duplicates << "\n";

  // Guards: the scenario really exercised what it claims to.
  EXPECT_GT(static_cast<double>(c.cancel_hits) / static_cast<double>(c.cancels), 0.05);
  EXPECT_GT(c.cross_symbol_duplicates, 0u);
  for (uint32_t s : {kA, kB, kC}) EXPECT_GT(c.trades_per_symbol[s], 0u) << "symbol " << s;
}

}  // namespace
