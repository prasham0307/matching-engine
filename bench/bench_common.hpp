#pragma once

// Pieces shared by the benchmarks (lob_latency and lob_pipeline): applying an operation to an
// OrderBook, the checksum over its trades, pinning a thread to a core, and machine information.
// Keeping one copy means both benchmarks compute the same checksum for the same operations.

#include <sched.h>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/order_generator.hpp"

namespace lob::bench {

inline std::optional<uint64_t> parse_u64(std::string_view s) {
  uint64_t v = 0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (s.empty() || ec != std::errc{} || ptr != end) return std::nullopt;
  return v;
}

inline std::optional<int64_t> parse_i64(std::string_view s) {
  int64_t v = 0;
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (s.empty() || ec != std::errc{} || ptr != end) return std::nullopt;
  return v;
}

inline std::string read_file(const char* path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string cpu_model() {
  std::istringstream in(read_file("/proc/cpuinfo"));
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("model name", 0) == 0) {
      const auto colon = line.find(':');
      if (colon != std::string::npos && colon + 2 <= line.size()) return line.substr(colon + 2);
    }
  }
  return "unknown";
}

inline bool running_under_wsl() {
  std::string v = read_file("/proc/version");
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v.find("microsoft") != std::string::npos || v.find("wsl") != std::string::npos;
}

inline bool pin_to_core(int core) {
  if (core < 0 || core >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}

struct Result {
  std::vector<Trade> trades;
  bool cancelled = false;
};

// Runs one operation into the caller's reusable Result. The order book clears r.trades itself.
inline void apply(OrderBook& book, const Op& op, Result& r) {
  r.cancelled = false;
  switch (op.kind) {
    case OpKind::Limit:
      book.submit_limit(op.id, op.side, op.price, op.qty, r.trades);
      break;
    case OpKind::Market:
      book.submit_market(op.id, op.side, op.qty, r.trades);
      break;
    case OpKind::Cancel:
      r.trades.clear();
      r.cancelled = book.cancel(op.id);
      break;
    case OpKind::Modify:
      book.modify(op.id, op.price, op.qty, r.trades);
      break;
  }
}

// Order-sensitive so a change in matching order or results changes the checksum.
inline uint64_t fold(uint64_t checksum, const Result& r) {
  for (const Trade& t : r.trades) {
    checksum = (checksum + static_cast<uint64_t>(t.price) * t.qty) ^ t.makerId ^ (t.takerId << 32);
  }
  if (r.cancelled) checksum ^= 0x9e3779b97f4a7c15ULL;
  return checksum;
}

}  // namespace lob::bench
