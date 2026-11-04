#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// The price levels of one side of the book (bids when kBid, asks otherwise).
//
// In-window prices, [base, base + window), live in a contiguous array of PriceLevel indexed by
// price - base. A bitmap with one bit per level (1 = non-empty) and a cursor holding the index of
// the best non-empty level (highest for bids, lowest for asks) make "what is the best price" an
// array read, and "what is the next best after this level empties" a few word scans with
// countl_zero / countr_zero. Nothing here allocates once the object is constructed, as long as
// every price is in the window.
//
// Prices outside the window are still fully supported, through a std::map used only for them.
// THAT PATH IS A SLOW PATH: it allocates a node per level, costs O(log n), and exists for
// correctness, not speed. best() merges the two structures by comparing the window's best price
// with the map's best price, so matching walks the levels of both in correct price order.
// out_of_window_rests() counts the orders that went to the map; while it is 0 the map has never
// been populated and none of the fallback code has run.
//
// Cost of a sparse window: a bigger window costs memory (24 bytes per tick, 1 bit per tick of
// bitmap) and, when the best level empties, up to window/64 bitmap word reads to find the next
// one. Occupied levels that sit close together share cache lines whatever the window size.
template <bool kBid>
class PriceLevels {
 public:
  static constexpr std::size_t kNone = ~std::size_t{0};

  // `window_ticks` may be 0, which sends every price to the fallback map.
  PriceLevels(int64_t base_price, std::size_t window_ticks)
      : base_(base_price),
        window_(window_ticks),
        levels_(window_ticks),
        bits_((window_ticks + 63) / 64, 0) {}

  int64_t base() const { return base_; }
  std::size_t window() const { return window_; }

  bool empty() const { return best_idx_ == kNone && overflow_.empty(); }

  // Best level on this side, or nullptr if the side is empty. `price` is set to its price.
  PriceLevel* best(int64_t& price) {
    if (best_idx_ != kNone) {
      const int64_t window_price = price_at(best_idx_);
      if (overflow_.empty() || better(window_price, overflow_.begin()->first)) {
        price = window_price;
        return &levels_[best_idx_];
      }
    } else if (overflow_.empty()) {
      return nullptr;
    }
    const auto it = overflow_.begin();  // fallback: the map's best is the side's best
    price = it->first;
    return &it->second;
  }

  std::optional<int64_t> best_price() const {
    std::optional<int64_t> result;
    if (best_idx_ != kNone) result = price_at(best_idx_);
    if (!overflow_.empty() && (!result || better(overflow_.begin()->first, *result))) {
      result = overflow_.begin()->first;
    }
    return result;
  }

  // The level for `price`, creating it if needed. Call it immediately before push_back: an empty
  // in-window level is marked non-empty here. Out-of-window prices take the slow path and may
  // allocate.
  PriceLevel& acquire(int64_t price) {
    if (in_window(price)) {
      const std::size_t idx = index_of(price);
      PriceLevel& level = levels_[idx];
      if (level.head == nullptr) {
        bits_[idx >> 6] |= uint64_t{1} << (idx & 63);
        if (best_idx_ == kNone || (kBid ? idx > best_idx_ : idx < best_idx_)) best_idx_ = idx;
      }
      return level;
    }
    ++out_of_window_rests_;
    return overflow_[price];
  }

  // The existing level at `price`, or nullptr if the map has none. An in-window price always has
  // a level object, which may be empty (totalQty 0).
  PriceLevel* find(int64_t price) {
    if (in_window(price)) return &levels_[index_of(price)];
    const auto it = overflow_.find(price);
    return it == overflow_.end() ? nullptr : &it->second;
  }
  const PriceLevel* find(int64_t price) const {
    if (in_window(price)) return &levels_[index_of(price)];
    const auto it = overflow_.find(price);
    return it == overflow_.end() ? nullptr : &it->second;
  }

  // Call after the last order has been unlinked from the level at `price`. Advances the best
  // cursor if this was the best level, or erases the map node for an out-of-window price.
  void release(int64_t price) {
    if (in_window(price)) {
      const std::size_t idx = index_of(price);
      bits_[idx >> 6] &= ~(uint64_t{1} << (idx & 63));
      if (idx == best_idx_) best_idx_ = next_after(idx);
    } else {
      overflow_.erase(price);
    }
  }

  // Orders that came to rest at an out-of-window price (slow path). Cumulative.
  std::size_t out_of_window_rests() const { return out_of_window_rests_; }
  // Out-of-window levels currently held in the fallback map.
  std::size_t overflow_levels() const { return overflow_.size(); }

  // Every non-empty level, best price first, merging the window and the fallback map. Allocates:
  // for display and for tests, not for the hot path.
  std::vector<std::pair<int64_t, const PriceLevel*>> snapshot() const {
    std::vector<std::pair<int64_t, const PriceLevel*>> in_window_levels;
    if constexpr (kBid) {
      for (std::size_t w = bits_.size(); w-- > 0;) {
        for (uint64_t word = bits_[w]; word != 0;) {
          const std::size_t idx = (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word));
          in_window_levels.emplace_back(price_at(idx), &levels_[idx]);
          word &= ~(uint64_t{1} << (idx & 63));
        }
      }
    } else {
      for (std::size_t w = 0; w < bits_.size(); ++w) {
        for (uint64_t word = bits_[w]; word != 0; word &= word - 1) {
          const std::size_t idx = (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
          in_window_levels.emplace_back(price_at(idx), &levels_[idx]);
        }
      }
    }

    std::vector<std::pair<int64_t, const PriceLevel*>> merged;
    merged.reserve(in_window_levels.size() + overflow_.size());
    std::size_t i = 0;
    auto it = overflow_.begin();
    while (i < in_window_levels.size() || it != overflow_.end()) {
      if (it == overflow_.end() ||
          (i < in_window_levels.size() && better(in_window_levels[i].first, it->first))) {
        merged.push_back(in_window_levels[i++]);
      } else {
        merged.emplace_back(it->first, &it->second);
        ++it;
      }
    }
    return merged;
  }

  // Checks the window bitmap, the best cursor and the fallback map against the levels they
  // describe. O(window). For tests and debugging only.
  bool check_structure() const {
    if (levels_.size() != window_ || bits_.size() != (window_ + 63) / 64) return false;

    std::size_t expected_best = kNone;
    for (std::size_t idx = 0; idx < window_; ++idx) {
      const PriceLevel& level = levels_[idx];
      const bool bit = (bits_[idx >> 6] >> (idx & 63)) & 1;
      if (bit != (level.head != nullptr)) return false;  // the bit must mirror non-emptiness
      if (level.head == nullptr && (level.tail != nullptr || level.totalQty != 0)) return false;
      if (level.head != nullptr && level.tail == nullptr) return false;
      // Scanning upward: for bids the last set bit is the best, for asks the first one is.
      if (bit && (kBid || expected_best == kNone)) expected_best = idx;
    }
    // Padding bits past the window must stay clear.
    if (window_ % 64 != 0 && !bits_.empty() && (bits_.back() >> (window_ % 64)) != 0) return false;
    if (best_idx_ != expected_best) return false;

    for (const auto& [price, level] : overflow_) {
      if (in_window(price)) return false;     // the map holds out-of-window prices only
      if (level.head == nullptr) return false;  // and never an empty level
    }
    return true;
  }

 private:
  using OverflowMap =
      std::map<int64_t, PriceLevel, std::conditional_t<kBid, std::greater<int64_t>, std::less<int64_t>>>;

  static bool better(int64_t a, int64_t b) { return kBid ? a > b : a < b; }

  // One unsigned compare; the subtraction wraps, so it is right for any pair of int64 values.
  bool in_window(int64_t price) const {
    return static_cast<uint64_t>(price) - static_cast<uint64_t>(base_) < window_;
  }
  std::size_t index_of(int64_t price) const {
    return static_cast<std::size_t>(static_cast<uint64_t>(price) - static_cast<uint64_t>(base_));
  }
  int64_t price_at(std::size_t idx) const {
    return static_cast<int64_t>(static_cast<uint64_t>(base_) + idx);
  }

  // The next non-empty level strictly worse than `idx` (lower for bids, higher for asks), or
  // kNone. Scans the bitmap a word at a time.
  std::size_t next_after(std::size_t idx) const {
    std::size_t w = idx >> 6;
    const unsigned b = idx & 63;
    if constexpr (kBid) {
      uint64_t word = bits_[w] & ((uint64_t{1} << b) - 1);  // bits below idx
      for (;;) {
        if (word != 0) return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word));
        if (w == 0) return kNone;
        word = bits_[--w];
      }
    } else {
      uint64_t word = bits_[w] & ~((uint64_t{2} << b) - 1);  // bits above idx
      for (;;) {
        if (word != 0) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
        if (++w == bits_.size()) return kNone;
        word = bits_[w];
      }
    }
  }

  int64_t base_;
  std::size_t window_;
  std::vector<PriceLevel> levels_;  // levels_[price - base_]
  std::vector<uint64_t> bits_;      // bit i set iff levels_[i] is non-empty
  std::size_t best_idx_ = kNone;    // index of the best non-empty in-window level
  OverflowMap overflow_;            // slow path: out-of-window prices only
  std::size_t out_of_window_rests_ = 0;
};

}  // namespace lob
