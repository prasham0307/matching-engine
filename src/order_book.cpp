#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

template <class Map>
uint32_t OrderBook::match_against(Map& book, uint64_t takerId, bool takerIsBuy,
                                  std::optional<int64_t> limit, uint32_t qty,
                                  std::vector<Trade>& out) {
  while (qty > 0 && !book.empty()) {
    const auto it = book.begin();  // best price on this side
    if (limit.has_value()) {
      const bool crosses = takerIsBuy ? (it->first <= *limit) : (it->first >= *limit);
      if (!crosses) break;
    }

    PriceLevel& level = it->second;
    while (qty > 0 && level.head != nullptr) {
      Order* maker = level.head;
      const uint32_t fill = std::min(qty, maker->remaining);
      out.push_back(Trade{maker->id, takerId, maker->price, fill});
      qty -= fill;
      maker->remaining -= fill;
      level.totalQty -= fill;
      if (maker->remaining == 0) {
        const uint64_t makerId = maker->id;  // copy: erase destroys the Order
        level.unlink(maker);
        orders_.erase(makerId);
      }
    }
    if (level.head == nullptr) book.erase(it);  // never leave an empty level behind
  }
  return qty;
}

void OrderBook::rest(uint64_t id, Side side, int64_t price, uint32_t qty, uint32_t remaining) {
  auto owned = std::make_unique<Order>(Order{id, 0, side, price, qty, remaining, ++seq_});
  Order* o = owned.get();
  if (side == Side::Buy) {
    bids_[price].push_back(o);
  } else {
    asks_[price].push_back(o);
  }
  orders_.emplace(id, std::move(owned));
}

std::vector<Trade> OrderBook::submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty) {
  std::vector<Trade> trades;
  if (qty == 0 || orders_.contains(id)) return trades;

  const uint32_t left = (side == Side::Buy)
                            ? match_against(asks_, id, true, price, qty, trades)
                            : match_against(bids_, id, false, price, qty, trades);
  if (left > 0) rest(id, side, price, qty, left);
  return trades;
}

std::vector<Trade> OrderBook::submit_market(uint64_t id, Side side, uint32_t qty) {
  std::vector<Trade> trades;
  if (qty == 0 || orders_.contains(id)) return trades;

  // Unfilled remainder is dropped, never rested.
  if (side == Side::Buy) {
    match_against(asks_, id, true, std::nullopt, qty, trades);
  } else {
    match_against(bids_, id, false, std::nullopt, qty, trades);
  }
  return trades;
}

bool OrderBook::cancel(uint64_t id) {
  const auto found = orders_.find(id);
  if (found == orders_.end()) return false;

  Order* o = found->second.get();
  if (o->side == Side::Buy) {
    const auto it = bids_.find(o->price);
    it->second.unlink(o);
    if (it->second.head == nullptr) bids_.erase(it);
  } else {
    const auto it = asks_.find(o->price);
    it->second.unlink(o);
    if (it->second.head == nullptr) asks_.erase(it);
  }
  orders_.erase(found);
  return true;
}

std::vector<Trade> OrderBook::modify(uint64_t id, int64_t new_price, uint32_t new_qty) {
  const auto found = orders_.find(id);
  if (found == orders_.end()) return {};

  const Side side = found->second->side;
  cancel(id);
  return submit_limit(id, side, new_price, new_qty);  // qty 0 is rejected: order stays cancelled
}

std::optional<int64_t> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<int64_t> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

uint32_t OrderBook::total_qty_at(Side side, int64_t price) const {
  if (side == Side::Buy) {
    const auto it = bids_.find(price);
    return it == bids_.end() ? 0u : static_cast<uint32_t>(it->second.totalQty);
  }
  const auto it = asks_.find(price);
  return it == asks_.end() ? 0u : static_cast<uint32_t>(it->second.totalQty);
}

std::vector<std::pair<int64_t, uint32_t>> OrderBook::depth(Side side) const {
  std::vector<std::pair<int64_t, uint32_t>> levels;
  if (side == Side::Buy) {
    levels.reserve(bids_.size());
    for (const auto& [price, level] : bids_) {
      levels.emplace_back(price, static_cast<uint32_t>(level.totalQty));
    }
  } else {
    levels.reserve(asks_.size());
    for (const auto& [price, level] : asks_) {
      levels.emplace_back(price, static_cast<uint32_t>(level.totalQty));
    }
  }
  return levels;
}

template <class Map>
bool OrderBook::check_side(const Map& book, Side side, std::size_t& counted) const {
  for (const auto& [price, level] : book) {
    // No empty levels, and the list ends must be consistent.
    if (level.head == nullptr || level.tail == nullptr) return false;
    if (level.head->prev != nullptr || level.tail->next != nullptr) return false;

    uint64_t sum = 0;
    const Order* prev = nullptr;
    for (const Order* o = level.head; o != nullptr; o = o->next) {
      if (++counted > orders_.size()) return false;  // cycle, or orders missing from the map
      const auto it = orders_.find(o->id);
      if (it == orders_.end() || it->second.get() != o) return false;
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

  // Every order in the id map must have been reached through exactly one level.
  if (counted != orders_.size()) return false;
  for (const auto& [id, order] : orders_) {
    if (order == nullptr || order->id != id) return false;
  }

  // The book is never crossed.
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
    return false;
  }
  return true;
}

}  // namespace lob
