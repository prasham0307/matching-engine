#pragma once

#include <ostream>

#include "lob/types.hpp"

namespace lob {

// Test-only helpers so Trade can be compared and printed in assertion messages.
inline bool operator==(const Trade& a, const Trade& b) {
  return a.makerId == b.makerId && a.takerId == b.takerId && a.price == b.price && a.qty == b.qty;
}
inline std::ostream& operator<<(std::ostream& os, const Trade& t) {
  return os << "(maker " << t.makerId << ", taker " << t.takerId << ", " << t.price << ", "
            << t.qty << ")";
}

}  // namespace lob
