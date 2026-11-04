#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "lob/id_index.hpp"
#include "lob/order_pool.hpp"
#include "lob/types.hpp"

namespace lob {

// Single-symbol limit order book. Price levels are ordered maps; each level is an intrusive
// doubly linked FIFO of resting orders, and the orders themselves come from an OrderPool. Same
// public interface and rules as NaiveMatcher, which it is tested against.
class OrderBook {
 public:
  // `expected_resting_orders` sizes the id index so it never has to grow while the book holds
  // that many orders or fewer. Growing is still correct, just a slow path that allocates.
  explicit OrderBook(std::size_t expected_resting_orders = 1024)
      : index_(expected_resting_orders) {}

  // Hot-path API. Trades are written to `out`, which is cleared first and keeps its capacity, so
  // a buffer reused across calls stops allocating once it has grown to the largest result.
  // A rejected or non-matching call leaves `out` empty.
  void submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty,
                    std::vector<Trade>& out);
  void submit_market(uint64_t id, Side side, uint32_t qty, std::vector<Trade>& out);
  void modify(uint64_t id, int64_t new_price, uint32_t new_qty, std::vector<Trade>& out);

  bool cancel(uint64_t id);

  // Convenience wrappers that return a fresh vector. They allocate; use the overloads above on
  // hot paths.
  std::vector<Trade> submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty) {
    std::vector<Trade> out;
    submit_limit(id, side, price, qty, out);
    return out;
  }
  std::vector<Trade> submit_market(uint64_t id, Side side, uint32_t qty) {
    std::vector<Trade> out;
    submit_market(id, side, qty, out);
    return out;
  }
  std::vector<Trade> modify(uint64_t id, int64_t new_price, uint32_t new_qty) {
    std::vector<Trade> out;
    modify(id, new_price, new_qty, out);
    return out;
  }

  std::optional<int64_t> best_bid() const;
  std::optional<int64_t> best_ask() const;
  uint32_t total_qty_at(Side side, int64_t price) const;

  // (price, total qty) for every level on one side, best price first. Read-only, for display.
  std::vector<std::pair<int64_t, uint32_t>> depth(Side side) const;

  bool is_resting(uint64_t id) const { return index_.contains(id); }

  // Id index statistics, so a benchmark can confirm the index never grew while it was timing.
  std::size_t resting_orders() const { return index_.size(); }
  std::size_t id_index_capacity() const { return index_.capacity(); }
  std::size_t id_index_resizes() const { return index_.resize_count(); }

  // Verifies the structural invariants of the book. For tests and debugging only.
  bool check_invariants() const;

 private:
  struct PriceLevel {
    Order* head = nullptr;
    Order* tail = nullptr;
    uint64_t totalQty = 0;

    void push_back(Order* o) {
      o->prev = tail;
      o->next = nullptr;
      if (tail != nullptr) {
        tail->next = o;
      } else {
        head = o;
      }
      tail = o;
      totalQty += o->remaining;
    }

    // Removes o from the list in O(1) and takes its remaining quantity off the total.
    void unlink(Order* o) {
      if (o->prev != nullptr) {
        o->prev->next = o->next;
      } else {
        head = o->next;
      }
      if (o->next != nullptr) {
        o->next->prev = o->prev;
      } else {
        tail = o->prev;
      }
      totalQty -= o->remaining;
      o->prev = nullptr;
      o->next = nullptr;
    }
  };

  using BidMap = std::map<int64_t, PriceLevel, std::greater<int64_t>>;
  using AskMap = std::map<int64_t, PriceLevel>;

  // Matches an incoming order against one side of the book (`book` is the opposite side).
  // `limit` empty means a market order. Returns the unfilled quantity.
  template <class Map>
  uint32_t match_against(Map& book, uint64_t takerId, bool takerIsBuy,
                         std::optional<int64_t> limit, uint32_t qty, std::vector<Trade>& out);

  template <class Map>
  bool check_side(const Map& book, Side side, std::size_t& counted) const;

  void rest(uint64_t id, Side side, int64_t price, uint32_t qty, uint32_t remaining);

  OrderPool pool_;  // owns the storage of every resting Order
  BidMap bids_;
  AskMap asks_;
  IdIndex index_;  // id -> Order*; the Orders themselves live in pool_
  uint64_t seq_ = 0;
};

}  // namespace lob
