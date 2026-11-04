#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

template <class Levels>
uint32_t OrderBook::match_against(Levels& book, uint64_t takerId, bool takerIsBuy,
                                  std::optional<int64_t> limit, uint32_t qty,
                                  std::vector<Trade>& out) {
  int64_t price = 0;
  while (qty > 0) {
    PriceLevel* level = book.best(price);  // best price on this side, window or fallback
    if (level == nullptr) break;
    if (limit.has_value()) {
      const bool crosses = takerIsBuy ? (price <= *limit) : (price >= *limit);
      if (!crosses) break;
    }

    while (qty > 0 && level->head != nullptr) {
      Order* maker = level->head;
      const uint32_t fill = std::min(qty, maker->remaining);
      out.push_back(Trade{maker->id, takerId, maker->price, fill});
      qty -= fill;
      maker->remaining -= fill;
      level->totalQty -= fill;
      if (maker->remaining == 0) {
        const uint64_t makerId = maker->id;  // copy: the slot is recycled right after
        level->unlink(maker);
        index_.erase(makerId);
        pool_.release(maker);
      }
    }
    if (level->head == nullptr) book.release(price);  // never leave an empty level behind
  }
  return qty;
}

void OrderBook::rest(uint64_t id, Side side, int64_t price, uint32_t qty, uint32_t remaining) {
  Order* o = pool_.allocate(Order{id, 0, side, price, qty, remaining, ++seq_});
  if (side == Side::Buy) {
    bids_.acquire(price).push_back(o);
  } else {
    asks_.acquire(price).push_back(o);
  }
  index_.insert(id, o);
}

void OrderBook::submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty,
                             std::vector<Trade>& out) {
  out.clear();
  if (qty == 0 || index_.contains(id)) return;

  const uint32_t left = (side == Side::Buy)
                            ? match_against(asks_, id, true, price, qty, out)
                            : match_against(bids_, id, false, price, qty, out);
  if (left > 0) rest(id, side, price, qty, left);
}

void OrderBook::submit_market(uint64_t id, Side side, uint32_t qty, std::vector<Trade>& out) {
  out.clear();
  if (qty == 0 || index_.contains(id)) return;

  // Unfilled remainder is dropped, never rested.
  if (side == Side::Buy) {
    match_against(asks_, id, true, std::nullopt, qty, out);
  } else {
    match_against(bids_, id, false, std::nullopt, qty, out);
  }
}

bool OrderBook::cancel(uint64_t id) {
  Order* o = index_.find(id);
  if (o == nullptr) return false;

  if (o->side == Side::Buy) {
    PriceLevel* level = bids_.find(o->price);
    level->unlink(o);
    if (level->head == nullptr) bids_.release(o->price);
  } else {
    PriceLevel* level = asks_.find(o->price);
    level->unlink(o);
    if (level->head == nullptr) asks_.release(o->price);
  }
  index_.erase(id);
  pool_.release(o);
  return true;
}

void OrderBook::modify(uint64_t id, int64_t new_price, uint32_t new_qty,
                       std::vector<Trade>& out) {
  out.clear();
  const Order* found = index_.find(id);
  if (found == nullptr) return;

  const Side side = found->side;
  cancel(id);
  submit_limit(id, side, new_price, new_qty, out);  // qty 0 is rejected: order stays cancelled
}

std::optional<int64_t> OrderBook::best_bid() const { return bids_.best_price(); }

std::optional<int64_t> OrderBook::best_ask() const { return asks_.best_price(); }

uint32_t OrderBook::total_qty_at(Side side, int64_t price) const {
  const PriceLevel* level = side == Side::Buy ? bids_.find(price) : asks_.find(price);
  return level == nullptr ? 0u : static_cast<uint32_t>(level->totalQty);
}

std::vector<std::pair<int64_t, uint32_t>> OrderBook::depth(Side side) const {
  const auto snapshot = side == Side::Buy ? bids_.snapshot() : asks_.snapshot();
  std::vector<std::pair<int64_t, uint32_t>> levels;
  levels.reserve(snapshot.size());
  for (const auto& [price, level] : snapshot) {
    levels.emplace_back(price, static_cast<uint32_t>(level->totalQty));
  }
  return levels;
}

template <class Levels>
bool OrderBook::check_side(const Levels& book, Side side, std::size_t& counted) const {
  if (!book.check_structure()) return false;  // bitmap, best cursor and fallback map
  for (const auto& [price, level_ptr] : book.snapshot()) {
    const PriceLevel& level = *level_ptr;
    // No empty levels, and the list ends must be consistent.
    if (level.head == nullptr || level.tail == nullptr) return false;
    if (level.head->prev != nullptr || level.tail->next != nullptr) return false;

    uint64_t sum = 0;
    const Order* prev = nullptr;
    for (const Order* o = level.head; o != nullptr; o = o->next) {
      if (++counted > index_.size()) return false;  // cycle, or orders missing from the index
      if (index_.find(o->id) != o) return false;
      if (o->side != side || o->price != price) return false;
      if (o->remaining == 0 || o->remaining > o->qty) return false;
      if (o->prev != prev) return false;  // backward link must mirror the forward walk
      sum += o->remaining;
      prev = o;
    }
    if (prev != level.tail) return false;
    if (sum != level.totalQty) return false;
  }
  return true;
}

bool OrderBook::check_invariants() const {
  std::size_t counted = 0;
  if (!check_side(bids_, Side::Buy, counted)) return false;
  if (!check_side(asks_, Side::Sell, counted)) return false;

  // The index itself must be structurally sound.
  if (!index_.check_invariants()) return false;
  // Every order in the id index must have been reached through exactly one level.
  if (counted != index_.size()) return false;
  // The pool must hold exactly the orders the id index knows about: nothing leaked or freed early.
  if (pool_.in_use() != index_.size()) return false;
  bool ids_match = true;
  index_.for_each([&ids_match](uint64_t id, const Order* order) {
    if (order == nullptr || order->id != id) ids_match = false;
  });
  if (!ids_match) return false;

  // The book is never crossed.
  const auto bid = bids_.best_price();
  const auto ask = asks_.best_price();
  if (bid && ask && *bid >= *ask) return false;
  return true;
}

}  // namespace lob
