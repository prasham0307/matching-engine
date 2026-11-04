#include "lob/price_levels.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace {

using lob::Order;
using lob::PriceLevel;
using lob::PriceLevels;
using lob::Side;

// Places and removes orders the way OrderBook does, on one side of the book.
template <bool kBid>
class Fixture {
 public:
  Fixture(int64_t base, std::size_t window) : levels(base, window) {}

  Order* add(int64_t price, uint32_t qty = 1) {
    orders_.push_back(Order{next_id_++, 0, kBid ? Side::Buy : Side::Sell, price, qty, qty, 0});
    Order* o = &orders_.back();
    levels.acquire(price).push_back(o);
    return o;
  }

  void remove(Order* o) {
    PriceLevel* level = levels.find(o->price);
    ASSERT_NE(level, nullptr);
    level->unlink(o);
    if (level->head == nullptr) levels.release(o->price);
  }

  std::optional<int64_t> best() const { return levels.best_price(); }

  PriceLevels<kBid> levels;

 private:
  std::deque<Order> orders_;  // stable addresses
  uint64_t next_id_ = 1;
};

// Each scenario is written once and run for both sides.
#define BOTH_SIDES(fn) \
  TEST(PriceLevels, fn) { \
    fn##_impl<true>();    \
    fn##_impl<false>();   \
  }

template <bool kBid>
void EmptySideHasNoBest_impl() {
  Fixture<kBid> f(100, 64);
  int64_t price = 0;
  EXPECT_TRUE(f.levels.empty());
  EXPECT_EQ(f.best(), std::nullopt);
  EXPECT_EQ(f.levels.best(price), nullptr);
  EXPECT_TRUE(f.levels.snapshot().empty());
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(EmptySideHasNoBest)

template <bool kBid>
void BestIsHighestForBidsAndLowestForAsks_impl() {
  Fixture<kBid> f(100, 64);
  f.add(103);
  f.add(100);
  f.add(101);
  EXPECT_EQ(f.best(), kBid ? 103 : 100);
  int64_t price = 0;
  PriceLevel* level = f.levels.best(price);
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(price, kBid ? 103 : 100);
  EXPECT_EQ(level->head->price, price);
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(BestIsHighestForBidsAndLowestForAsks)

template <bool kBid>
void BestCursorAdvancesWhenTheBestLevelEmpties_impl() {
  Fixture<kBid> f(100, 64);
  Order* a = f.add(100);
  Order* b = f.add(101);
  Order* c = f.add(103);
  const std::vector<Order*> best_first = kBid ? std::vector<Order*>{c, b, a}
                                              : std::vector<Order*>{a, b, c};
  for (std::size_t i = 0; i < best_first.size(); ++i) {
    EXPECT_EQ(f.best(), best_first[i]->price);
    f.remove(best_first[i]);
    EXPECT_TRUE(f.levels.check_structure());
  }
  EXPECT_EQ(f.best(), std::nullopt);
  EXPECT_TRUE(f.levels.empty());
}
BOTH_SIDES(BestCursorAdvancesWhenTheBestLevelEmpties)

// Levels in different 64-bit bitmap words, including the first and last tick of the window.
template <bool kBid>
void BestCursorFindsTheNextLevelAcrossBitmapWords_impl() {
  Fixture<kBid> f(1000, 300);  // 5 words, the last one partly used
  std::vector<Order*> orders;
  for (const int64_t idx : {0, 3, 63, 64, 70, 191, 200, 299}) orders.push_back(f.add(1000 + idx));
  if (kBid) std::reverse(orders.begin(), orders.end());  // best first
  for (Order* o : orders) {
    EXPECT_EQ(f.best(), o->price);
    f.remove(o);
    ASSERT_TRUE(f.levels.check_structure());
  }
  EXPECT_EQ(f.best(), std::nullopt);
}
BOTH_SIDES(BestCursorFindsTheNextLevelAcrossBitmapWords)

template <bool kBid>
void RemovingANonBestLevelLeavesTheCursorAlone_impl() {
  Fixture<kBid> f(100, 64);
  Order* best = f.add(kBid ? 110 : 100);
  Order* middle = f.add(105);
  f.add(kBid ? 100 : 110);
  f.remove(middle);
  EXPECT_EQ(f.best(), best->price);
  EXPECT_TRUE(f.levels.check_structure());
  EXPECT_EQ(f.levels.snapshot().size(), 2u);
}
BOTH_SIDES(RemovingANonBestLevelLeavesTheCursorAlone)

template <bool kBid>
void ALevelStaysUntilItsLastOrderIsGone_impl() {
  Fixture<kBid> f(100, 64);
  Order* first = f.add(105, 3);
  Order* second = f.add(105, 4);
  EXPECT_EQ(f.levels.find(105)->totalQty, 7u);
  f.remove(first);
  EXPECT_EQ(f.best(), 105);
  EXPECT_EQ(f.levels.find(105)->totalQty, 4u);
  f.remove(second);
  EXPECT_EQ(f.best(), std::nullopt);
  EXPECT_EQ(f.levels.find(105)->totalQty, 0u);  // an in-window level object stays, empty
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(ALevelStaysUntilItsLastOrderIsGone)

// Window [100, 108): 100 and 107 are the first and last in-window ticks; 99 and 108 are the
// nearest out-of-window ticks. Only the latter two may count as slow-path rests.
template <bool kBid>
void WindowBoundariesAreExact_impl() {
  Fixture<kBid> f(100, 8);
  f.add(100);
  f.add(107);
  EXPECT_EQ(f.levels.out_of_window_rests(), 0u);
  EXPECT_EQ(f.levels.overflow_levels(), 0u);

  f.add(99);
  EXPECT_EQ(f.levels.out_of_window_rests(), 1u);
  f.add(108);
  EXPECT_EQ(f.levels.out_of_window_rests(), 2u);
  EXPECT_EQ(f.levels.overflow_levels(), 2u);

  for (const int64_t p : {99, 100, 107, 108}) {
    ASSERT_NE(f.levels.find(p), nullptr) << p;
    EXPECT_EQ(f.levels.find(p)->totalQty, 1u) << p;
  }
  EXPECT_EQ(f.levels.find(98), nullptr);   // out of window, no level
  EXPECT_EQ(f.levels.find(109), nullptr);
  EXPECT_NE(f.levels.find(101), nullptr);  // in window: always an object, here empty
  EXPECT_EQ(f.levels.find(101)->totalQty, 0u);
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(WindowBoundariesAreExact)

// Out-of-window prices that are better than every in-window price, and ones that are worse,
// must be merged with the window in the right order.
template <bool kBid>
void FallbackLevelsMergeWithTheWindowByPrice_impl() {
  Fixture<kBid> f(100, 8);
  Order* better_far = f.add(kBid ? 500 : 5);     // outside, better than the whole window
  Order* in_window = f.add(103);
  Order* worse_far = f.add(kBid ? 5 : 500);      // outside, worse than the whole window

  EXPECT_EQ(f.best(), better_far->price);
  const auto snap = f.levels.snapshot();
  ASSERT_EQ(snap.size(), 3u);
  EXPECT_EQ(snap[0].first, better_far->price);
  EXPECT_EQ(snap[1].first, 103);
  EXPECT_EQ(snap[2].first, worse_far->price);

  f.remove(better_far);
  EXPECT_EQ(f.best(), 103);
  f.remove(in_window);
  EXPECT_EQ(f.best(), worse_far->price);  // only the fallback is left
  f.remove(worse_far);
  EXPECT_EQ(f.best(), std::nullopt);
  EXPECT_TRUE(f.levels.empty());
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(FallbackLevelsMergeWithTheWindowByPrice)

template <bool kBid>
void ZeroWidthWindowSendsEverythingToTheFallback_impl() {
  Fixture<kBid> f(0, 0);
  Order* a = f.add(10);
  Order* b = f.add(20);
  EXPECT_EQ(f.levels.out_of_window_rests(), 2u);
  EXPECT_EQ(f.best(), kBid ? 20 : 10);
  f.remove(kBid ? b : a);
  EXPECT_EQ(f.best(), kBid ? 10 : 20);
  EXPECT_TRUE(f.levels.check_structure());
}
BOTH_SIDES(ZeroWidthWindowSendsEverythingToTheFallback)

// Window of 70 ticks: the second bitmap word is only partly used, and the bits past the window
// must stay clear.
TEST(PriceLevels, WindowThatIsNotAMultipleOf64Works) {
  {
    Fixture<true> f(0, 70);
    Order* last = f.add(69);
    f.add(0);
    f.add(70);
    EXPECT_EQ(f.best(), 70);  // fallback price above the window is the best bid
    EXPECT_TRUE(f.levels.check_structure());
    f.remove(last);
    EXPECT_EQ(f.best(), 70);
    EXPECT_TRUE(f.levels.check_structure());
  }
  {
    Fixture<false> f(0, 70);
    Order* first = f.add(0);
    f.add(69);
    f.add(70);
    EXPECT_EQ(f.best(), 0);
    f.remove(first);
    EXPECT_EQ(f.best(), 69);  // the last in-window tick, found in the second bitmap word
    EXPECT_TRUE(f.levels.check_structure());
  }
}

// The in-window test is an unsigned subtraction; it must hold at the ends of the int64 range.
TEST(PriceLevels, ExtremePricesAreHandledAroundTheWindow) {
  constexpr int64_t kMax = INT64_MAX;
  constexpr int64_t kMin = INT64_MIN;
  {
    Fixture<false> f(kMax - 3, 4);  // window = the top four ticks
    f.add(kMax);
    f.add(kMax - 3);
    f.add(kMin);  // far below: fallback, and the best ask
    EXPECT_EQ(f.levels.out_of_window_rests(), 1u);
    EXPECT_EQ(f.best(), kMin);
    EXPECT_TRUE(f.levels.check_structure());
  }
  {
    Fixture<true> f(kMin, 8);  // window = the bottom eight ticks
    f.add(kMin);
    f.add(kMin + 7);
    f.add(kMax);  // fallback, the best bid
    EXPECT_EQ(f.levels.out_of_window_rests(), 1u);
    EXPECT_EQ(f.best(), kMax);
    const auto snap = f.levels.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[1].first, kMin + 7);
    EXPECT_EQ(snap[2].first, kMin);
    EXPECT_TRUE(f.levels.check_structure());
  }
}

TEST(PriceLevels, NegativePricesWorkInsideAWindow) {
  Fixture<true> f(-5, 10);  // -5 .. 4
  f.add(-5);
  f.add(0);
  f.add(4);
  EXPECT_EQ(f.levels.out_of_window_rests(), 0u);
  EXPECT_EQ(f.best(), 4);
  EXPECT_TRUE(f.levels.check_structure());
}

// Random adds and removals on both sides, with prices on and around the window, checked against
// a std::map reference after every step.
template <bool kBid>
void run_randomized(int64_t base, std::size_t window, uint64_t seed) {
  Fixture<kBid> f(base, window);
  std::map<int64_t, uint64_t> reference_qty;  // price -> total quantity
  std::vector<Order*> live;
  std::mt19937_64 rng(seed);
  const int64_t lo = base - 40;
  const uint64_t span = window + 80;

  for (int step = 0; step < 20000; ++step) {
    const bool add = live.empty() || rng() % 100 < 55;
    if (add) {
      const int64_t price = lo + static_cast<int64_t>(rng() % span);
      const uint32_t qty = 1 + static_cast<uint32_t>(rng() % 5);
      live.push_back(f.add(price, qty));
      reference_qty[price] += qty;
    } else {
      const std::size_t victim = rng() % live.size();
      Order* o = live[victim];
      reference_qty[o->price] -= o->remaining;
      if (reference_qty[o->price] == 0) reference_qty.erase(o->price);
      f.remove(o);
      live[victim] = live.back();
      live.pop_back();
    }

    std::optional<int64_t> expected_best;
    if (!reference_qty.empty()) {
      expected_best = kBid ? reference_qty.rbegin()->first : reference_qty.begin()->first;
    }
    ASSERT_EQ(f.best(), expected_best) << "step " << step;
    if (step % 250 == 0) {
      ASSERT_TRUE(f.levels.check_structure()) << "step " << step;
      const auto snap = f.levels.snapshot();
      ASSERT_EQ(snap.size(), reference_qty.size()) << "step " << step;
      std::size_t i = 0;
      auto emit = [&](int64_t price, uint64_t qty) {
        ASSERT_EQ(snap[i].first, price) << "step " << step;
        ASSERT_EQ(snap[i].second->totalQty, qty) << "step " << step;
        ++i;
      };
      if (kBid) {
        for (auto it = reference_qty.rbegin(); it != reference_qty.rend(); ++it) emit(it->first, it->second);
      } else {
        for (const auto& [price, qty] : reference_qty) emit(price, qty);
      }
    }
  }
}

TEST(PriceLevels, RandomizedAgainstAMapReference) {
  struct Window {
    int64_t base;
    std::size_t ticks;
  };
  uint64_t seed = 100;
  for (const Window w : {Window{100, 1}, Window{100, 63}, Window{100, 64}, Window{100, 65},
                         Window{-30, 130}, Window{0, 0}, Window{1000, 300}}) {
    run_randomized<true>(w.base, w.ticks, seed++);
    run_randomized<false>(w.base, w.ticks, seed++);
  }
}

}  // namespace
