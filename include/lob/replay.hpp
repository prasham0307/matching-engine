#pragma once

// Text replay of order events through a MatchingEngine, with a trade log, a hash of that log, and
// an optional cross-check against NaiveMatcher. See README.md ("Replay") for the file format.

#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lob/matching_engine.hpp"
#include "lob/naive_matcher.hpp"
#include "lob/types.hpp"

namespace lob::replay {

enum class EventKind { Limit, Market, Cancel, Modify };

struct Event {
  EventKind kind = EventKind::Limit;
  uint64_t id = 0;
  uint32_t symbol = 0;       // Limit, Market
  Side side = Side::Buy;     // Limit, Market
  int64_t price = 0;         // Limit; the new price for Modify
  uint32_t qty = 0;          // Limit, Market; the new quantity for Modify
};

enum class ParseStatus { Event, Skip, Error };

// Parses one line of the replay format:
//   LIMIT,<id>,<symbol>,<BUY|SELL>,<price_ticks>,<qty>
//   MARKET,<id>,<symbol>,<BUY|SELL>,<qty>
//   CANCEL,<id>
//   MODIFY,<id>,<new_price_ticks>,<new_qty>
// Blank lines and lines starting with '#' are Skip. Spaces and tabs around fields, and a trailing
// '\r', are ignored. On Error, `error` says what is wrong with the line (the caller adds the line
// number).
ParseStatus parse_line(std::string_view line, Event& event, std::string& error);

// 64-bit FNV-1a.
class Fnv1a64 {
 public:
  void add(const char* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      hash_ ^= static_cast<unsigned char>(data[i]);
      hash_ *= 0x100000001b3ULL;
    }
  }
  void add(std::string_view s) { add(s.data(), s.size()); }
  uint64_t value() const { return hash_; }

 private:
  uint64_t hash_ = 0xcbf29ce484222325ULL;
};

// Writes `events` events over symbols 1..`symbols` from OrderGenerator with the given seed. The
// same arguments always produce the same text. The first line is a comment recording them.
void generate_events(std::ostream& out, uint64_t events, uint32_t symbols, uint64_t seed);

// The reference side of --verify: one NaiveMatcher per symbol, applying the same rules the
// MatchingEngine documents (ids are global, cancel and modify find the order wherever it rests).
class Oracle {
 public:
  struct Outcome {
    std::vector<Trade> trades;
    bool found = true;  // Cancel and Modify: whether the id was resting
  };
  Outcome apply(const Event& event);

 private:
  std::optional<uint32_t> owner_of(uint64_t id) const;

  std::map<uint32_t, NaiveMatcher> books_;
  uint64_t max_submitted_ = 0;  // largest id seen on a Limit or Market event
  bool any_submitted_ = false;
};

// Describes how two outcomes differ, or returns nothing if they are the same.
std::optional<std::string> diff_outcomes(const std::vector<Trade>& engine_trades, bool engine_found,
                                         const Oracle::Outcome& oracle);

struct ReplayOptions {
  bool verify = false;                // also run every event through the Oracle
  std::ostream* trade_log = nullptr;  // trade lines are written here if set (the hash is always computed)
  EngineConfig engine;                // sizing of the MatchingEngine
};

struct SymbolSummary {
  uint32_t symbol = 0;
  std::optional<int64_t> best_bid;
  std::optional<int64_t> best_ask;
  std::size_t resting = 0;
};

enum class Failure { None, Malformed, Mismatch, Io };

struct ReplayResult {
  Failure failure = Failure::None;
  uint64_t line = 0;        // line number of the failure (1-based)
  std::string message;      // what went wrong
  uint64_t lines = 0;       // lines read
  uint64_t events = 0;      // events processed
  uint64_t trades = 0;
  uint64_t rejected_orders = 0;   // limit or market: quantity 0, or the id was already resting
  uint64_t missed_cancels = 0;    // cancel of an id that was not resting
  uint64_t missed_modifies = 0;   // modify of an id that was not resting
  uint64_t trade_hash = 0;        // FNV-1a 64 of the trade log text
  std::size_t id_table_resizes = 0;
  std::vector<SymbolSummary> symbols;  // in the order symbols were first seen
  bool ok() const { return failure == Failure::None; }
};

// One trade log line, without the newline: maker id, taker id, symbol, price, qty.
// Returns the number of characters written to `buf`, which must hold at least 96.
std::size_t format_trade(char* buf, const Trade& trade, uint32_t symbol);

// Reads events from `in` until the end, stopping at the first malformed line, or the first
// engine/oracle disagreement when verifying.
ReplayResult replay(std::istream& in, const ReplayOptions& options);

}  // namespace lob::replay
