#include "lob/naive_matcher.hpp"

#include <algorithm>
#include <cstddef>

namespace lob {

bool NaiveMatcher::is_resting(uint64_t id) const {
  return std::any_of(book_.begin(), book_.end(), [id](const Order& o) { return o.id == id; });
}

uint32_t NaiveMatcher::match(uint64_t takerId, Side side, std::optional<int64_t> limit,
                             uint32_t qty, std::vector<Trade>& out) {
  constexpr std::size_t npos = static_cast<std::size_t>(-1);

  while (qty > 0) {
    // Find the best opposing order: best price first, then lowest seq.
    std::size_t best = npos;
    for (std::size_t i = 0; i < book_.size(); ++i) {
      const Order& o = book_[i];
      if (o.side == side) continue;

      if (limit.has_value()) {
        const bool crosses = (side == Side::Buy) ? (o.price <= *limit) : (o.price >= *limit);
        if (!crosses) continue;
      }

      if (best == npos) {
        best = i;
        continue;
      }
      const Order& b = book_[best];
      // A buyer wants the lowest ask, a seller wants the highest bid.
      const bool betterPrice = (side == Side::Buy) ? (o.price < b.price) : (o.price > b.price);
      const bool samePrice = (o.price == b.price);
      if (betterPrice || (samePrice && o.seq < b.seq)) best = i;
    }

    if (best == npos) break;

    Order& maker = book_[best];
    const uint32_t fill = std::min(qty, maker.remaining);
    out.push_back(Trade{maker.id, takerId, maker.price, fill});
    qty -= fill;
    maker.remaining -= fill;
    if (maker.remaining == 0) {
      book_.erase(book_.begin() + static_cast<std::ptrdiff_t>(best));
    }
  }
  return qty;
}

std::vector<Trade> NaiveMatcher::submit_limit(uint64_t id, Side side, int64_t price,
                                              uint32_t qty) {
  std::vector<Trade> trades;
  if (qty == 0 || is_resting(id)) return trades;

  const uint32_t left = match(id, side, price, qty, trades);
  if (left > 0) {
    book_.push_back(Order{id, 0, side, price, qty, left, ++seq_});
  }
  return trades;
}

std::vector<Trade> NaiveMatcher::submit_market(uint64_t id, Side side, uint32_t qty) {
  std::vector<Trade> trades;
  if (qty == 0 || is_resting(id)) return trades;

  match(id, side, std::nullopt, qty, trades);  // unfilled remainder is dropped
  return trades;
}

bool NaiveMatcher::cancel(uint64_t id) {
  const auto it =
      std::find_if(book_.begin(), book_.end(), [id](const Order& o) { return o.id == id; });
  if (it == book_.end()) return false;
  book_.erase(it);
  return true;
}

std::vector<Trade> NaiveMatcher::modify(uint64_t id, int64_t new_price, uint32_t new_qty) {
  const auto it =
      std::find_if(book_.begin(), book_.end(), [id](const Order& o) { return o.id == id; });
  if (it == book_.end()) return {};

  const Side side = it->side;
  book_.erase(it);
  return submit_limit(id, side, new_price, new_qty);
}

std::optional<int64_t> NaiveMatcher::best_bid() const {
  std::optional<int64_t> best;
  for (const Order& o : book_) {
    if (o.side == Side::Buy && (!best || o.price > *best)) best = o.price;
  }
  return best;
}

std::optional<int64_t> NaiveMatcher::best_ask() const {
  std::optional<int64_t> best;
  for (const Order& o : book_) {
    if (o.side == Side::Sell && (!best || o.price < *best)) best = o.price;
  }
  return best;
}

uint32_t NaiveMatcher::total_qty_at(Side side, int64_t price) const {
  uint32_t total = 0;
  for (const Order& o : book_) {
    if (o.side == side && o.price == price) total += o.remaining;
  }
  return total;
}

}  // namespace lob
