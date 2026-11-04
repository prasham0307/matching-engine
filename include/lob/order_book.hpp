#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Single-symbol limit order book, v1. Price levels are ordered maps; each level is an
// intrusive doubly linked FIFO of resting orders. Same public interface and rules as
// NaiveMatcher, which it is tested against.
class OrderBook {
 public:
  std::vector<Trade> submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty);
  std::vector<Trade> submit_market(uint64_t id, Side side, uint32_t qty);
  bool cancel(uint64_t id);
  std::vector<Trade> modify(uint64_t id, int64_t new_price, uint32_t new_qty);

  std::optional<int64_t> best_bid() const;
  std::optional<int64_t> best_ask() const;
  uint32_t total_qty_at(Side side, int64_t price) const;

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

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<uint64_t, std::unique_ptr<Order>> orders_;  // v1: heap allocated
  uint64_t seq_ = 0;
};

}  // namespace lob
