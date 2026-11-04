#pragma once

#include <cstdint>

namespace lob {

enum class Side : uint8_t { Buy, Sell };

// Prices are integer ticks. seq is an arrival counter used for time priority (never a clock).
struct Order {
  uint64_t id;
  uint32_t symbol;
  Side side;
  int64_t price;
  uint32_t qty;
  uint32_t remaining;
  uint64_t seq;
};

struct Trade {
  uint64_t makerId;
  uint64_t takerId;
  int64_t price;
  uint32_t qty;
};

}  // namespace lob
