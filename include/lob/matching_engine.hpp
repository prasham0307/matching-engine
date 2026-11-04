#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "lob/flat_map.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

struct EngineConfig {
  // Symbols expected, to size the symbol -> book table.
  std::size_t expected_symbols = 16;
  // Orders expected to rest at once across all symbols, to size the global id -> symbol table.
  std::size_t expected_resting_orders = 1024;
  // Orders expected to rest at once on one symbol, to size each new book's id index.
  std::size_t expected_resting_per_symbol = 1024;
  // Price window handed to every new book (see OrderBook).
  int64_t window_base = 0;
  std::size_t window_ticks = OrderBook::kDefaultWindowTicks;
};

// Routes orders to one OrderBook per symbol. Single-threaded and deterministic: the same sequence
// of calls always gives the same trades, and books are kept in the order their symbols were first
// seen.
//
// Order ids are GLOBAL. A limit or market order whose id is resting on any symbol is rejected as a
// duplicate, whichever symbol it names. This is what lets cancel(id) and modify(id, ...) find the
// right book without being told the symbol: the engine keeps a flat table from the id of every
// resting order to its symbol. An id stops being "taken" when its order is fully filled or
// cancelled, and can then be reused on any symbol.
//
// The table is kept up to date from the trades each call returns: a maker that is no longer
// resting after the call leaves it. That costs one is_resting() probe per trade.
//
// Allocation: the engine allocates only when it sees a symbol for the first time (a new
// OrderBook, plus its pool chunks and index), and when one of its two flat tables outgrows its
// presized capacity (a rare slow path, see FlatMap). Routing, matching, cancelling and modifying
// existing symbols allocates nothing here; OrderBook itself allocates nothing on the hot path.
//
// Trades carry no symbol. The symbol is the argument of submit_limit / submit_market, and for
// modify it is symbol_of(id), read before the call.
class MatchingEngine {
 public:
  explicit MatchingEngine(const EngineConfig& config = {});

  // A limit order for `symbol`. Creates the symbol's book on first sight. Rejected, with no
  // trades and no new book, if qty is 0 or the id is already resting on any symbol.
  void submit_limit(uint32_t symbol, uint64_t id, Side side, int64_t price, uint32_t qty,
                    std::vector<Trade>& out);

  // A market order for `symbol`. Never creates a book: an unknown symbol has no liquidity, so the
  // result is empty. Rejected like a limit order for qty 0 or an id that is already resting.
  void submit_market(uint32_t symbol, uint64_t id, Side side, uint32_t qty,
                     std::vector<Trade>& out);

  // Cancels the resting order `id`, wherever it is. False if no order with that id is resting.
  bool cancel(uint64_t id);

  // Cancels `id` only if it is resting on `symbol`. False for an unknown symbol, an unknown id,
  // or an id resting on a different symbol (which is left alone).
  bool cancel(uint32_t symbol, uint64_t id);

  // Cancel plus resubmit of the resting order `id` (it loses its queue position), on the symbol
  // it rests on. False, with `out` empty, if no order with that id is resting. A new quantity of
  // 0 cancels the order.
  bool modify(uint64_t id, int64_t new_price, uint32_t new_qty, std::vector<Trade>& out);

  // Symbol of a resting order, or nothing.
  std::optional<uint32_t> symbol_of(uint64_t id) const;

  bool has_symbol(uint32_t symbol) const { return symbol_slot_.contains(symbol); }
  // The book for `symbol`, or nullptr if the symbol has never been seen.
  const OrderBook* book(uint32_t symbol) const;
  // Symbols in the order they were first seen.
  const std::vector<uint32_t>& symbols() const { return symbols_; }

  std::optional<int64_t> best_bid(uint32_t symbol) const;
  std::optional<int64_t> best_ask(uint32_t symbol) const;
  uint32_t total_qty_at(uint32_t symbol, Side side, int64_t price) const;

  // Orders resting across all symbols.
  std::size_t resting_orders() const { return ids_.size(); }
  // Times the global id table had to grow (zero while within expected_resting_orders).
  std::size_t id_table_resizes() const { return ids_.resize_count(); }

  // Checks every book and that the global id table matches exactly what is resting. For tests.
  bool check_invariants() const;

 private:
  OrderBook* find_book(uint32_t symbol);
  OrderBook& book_for(uint32_t symbol);  // creates the book on first sight (allocates)
  // Brings the global id table in line with the result of one call on `symbol`'s book.
  void settle(const OrderBook& book, uint32_t symbol, uint64_t taker_id,
              const std::vector<Trade>& trades);

  EngineConfig config_;
  std::deque<OrderBook> books_;  // deque: books never move once created
  std::vector<uint32_t> symbols_;  // symbol of books_[i], in first-seen order
  FlatMap symbol_slot_;            // symbol -> index into books_
  FlatMap ids_;                    // id of every resting order -> its symbol
};

}  // namespace lob
