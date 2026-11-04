#pragma once

#include <algorithm>
#include <cstdint>
#include <random>

#include "lob/types.hpp"

namespace lob {

enum class OpKind { Limit, Market, Cancel, Modify };

// Fields that do not apply to a kind are zero/Buy: Market ignores price; Cancel uses only id;
// Modify uses id, price and qty (the side of the resting order is kept).
struct Op {
  OpKind kind;
  uint64_t id;
  Side side;
  int64_t price;
  uint32_t qty;
};

struct GenConfig {
  uint64_t seed = 1;
  int64_t price_min = 90;
  int64_t price_max = 110;
  uint32_t qty_min = 1;
  uint32_t qty_max = 20;
  uint32_t weight_limit = 55;
  uint32_t weight_market = 10;
  uint32_t weight_cancel = 25;
  uint32_t weight_modify = 10;
  // Cancel/modify targeting: after the ~5 percent never-issued case, recent_bias_pct percent
  // of targets are drawn uniformly from the last recent_window issued ids, the rest from all
  // ids ever issued. Recent ids are far more likely to still be resting.
  uint32_t recent_bias_pct = 80;
  uint32_t recent_window = 50;
};

// Deterministic random operation stream. Only the raw output of std::mt19937_64 (whose
// algorithm is fixed by the standard) is used, so a seed gives the same stream on every
// standard library. std::uniform_int_distribution is deliberately avoided: its algorithm
// is implementation defined.
class OrderGenerator {
 public:
  explicit OrderGenerator(const GenConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {}

  Op next() {
    const uint64_t total = static_cast<uint64_t>(cfg_.weight_limit) + cfg_.weight_market +
                           cfg_.weight_cancel + cfg_.weight_modify;
    uint64_t r = below(total);

    if (r < cfg_.weight_limit) {
      return Op{OpKind::Limit, ++last_id_, random_side(), random_price(), random_qty()};
    }
    r -= cfg_.weight_limit;
    if (r < cfg_.weight_market) {
      return Op{OpKind::Market, ++last_id_, random_side(), 0, random_qty()};
    }
    r -= cfg_.weight_market;
    if (r < cfg_.weight_cancel) {
      return Op{OpKind::Cancel, target_id(), Side::Buy, 0, 0};
    }
    return Op{OpKind::Modify, target_id(), Side::Buy, random_price(), random_qty()};
  }

 private:
  // Uniform in [0, n), n > 0. Rejection sampling removes modulo bias.
  uint64_t below(uint64_t n) {
    const uint64_t threshold = (0 - n) % n;  // 2^64 mod n
    uint64_t r = rng_();
    while (r < threshold) r = rng_();
    return r % n;
  }

  Side random_side() { return below(2) == 0 ? Side::Buy : Side::Sell; }

  int64_t random_price() {
    const uint64_t span = static_cast<uint64_t>(cfg_.price_max - cfg_.price_min) + 1;
    return cfg_.price_min + static_cast<int64_t>(below(span));
  }

  uint32_t random_qty() {
    const uint64_t span = static_cast<uint64_t>(cfg_.qty_max - cfg_.qty_min) + 1;
    return cfg_.qty_min + static_cast<uint32_t>(below(span));
  }

  // About 5 percent of the time an id that has not been issued yet. Otherwise, with
  // recent_bias_pct percent probability one of the last recent_window issued ids, else any id
  // ever issued (filled and cancelled ones included).
  uint64_t target_id() {
    if (last_id_ == 0 || below(100) < 5) return last_id_ + 1 + below(1000);
    const uint64_t window = std::min<uint64_t>(cfg_.recent_window, last_id_);
    if (window > 0 && below(100) < cfg_.recent_bias_pct) return last_id_ - below(window);
    return 1 + below(last_id_);
  }

  GenConfig cfg_;
  std::mt19937_64 rng_;
  uint64_t last_id_ = 0;
};

}  // namespace lob
