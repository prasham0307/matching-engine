#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Reference matcher for a single symbol. Deliberately simple and slow: every operation
// scans one flat vector of resting orders. Its job is to be obviously correct so that
// faster implementations can be fuzzed against it.
class NaiveMatcher {
 public:
  std::vector<Trade> submit_limit(uint64_t id, Side side, int64_t price, uint32_t qty);
  std::vector<Trade> submit_market(uint64_t id, Side side, uint32_t qty);
  bool cancel(uint64_t id);
  std::vector<Trade> modify(uint64_t id, int64_t new_price, uint32_t new_qty);

  std::optional<int64_t> best_bid() const;
  std::optional<int64_t> best_ask() const;
  uint32_t total_qty_at(Side side, int64_t price) const;

 private:
  bool is_resting(uint64_t id) const;

  // Matches an incoming order against the book, appending trades to `out`.
  // `limit` empty means a market order. Returns the unfilled quantity.
  uint32_t match(uint64_t takerId, Side side, std::optional<int64_t> limit, uint32_t qty,
                 std::vector<Trade>& out);

  std::vector<Order> book_;
  uint64_t seq_ = 0;
};

}  // namespace lob
