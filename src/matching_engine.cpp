#include "lob/matching_engine.hpp"

namespace lob {

MatchingEngine::MatchingEngine(const EngineConfig& config)
    : config_(config),
      symbol_slot_(config.expected_symbols),
      ids_(config.expected_resting_orders) {
  symbols_.reserve(config.expected_symbols);
}

OrderBook* MatchingEngine::find_book(uint32_t symbol) {
  const auto slot = symbol_slot_.find(symbol);
  return slot ? &books_[*slot] : nullptr;
}

const OrderBook* MatchingEngine::book(uint32_t symbol) const {
  const auto slot = symbol_slot_.find(symbol);
  return slot ? &books_[*slot] : nullptr;
}

OrderBook& MatchingEngine::book_for(uint32_t symbol) {
  if (OrderBook* existing = find_book(symbol)) return *existing;
  // First sight of this symbol: the only place the engine allocates on the routing path.
  books_.emplace_back(config_.expected_resting_per_symbol, config_.window_base,
                      config_.window_ticks);
  symbols_.push_back(symbol);
  symbol_slot_.insert(symbol, static_cast<uint32_t>(books_.size() - 1));
  return books_.back();
}

void MatchingEngine::settle(const OrderBook& book, uint32_t symbol, uint64_t taker_id,
                            const std::vector<Trade>& trades) {
  // A maker that is no longer resting was filled completely by this call.
  for (const Trade& t : trades) {
    if (!book.is_resting(t.makerId)) ids_.erase(t.makerId);
  }
  // The taker is taken only while it rests (a market order never does).
  if (book.is_resting(taker_id)) {
    ids_.insert(taker_id, symbol);  // already present for a modify; insert then leaves it alone
  } else {
    ids_.erase(taker_id);
  }
}

void MatchingEngine::submit_limit(uint32_t symbol, uint64_t id, Side side, int64_t price,
                                  uint32_t qty, std::vector<Trade>& out) {
  out.clear();
  if (qty == 0 || ids_.contains(id)) return;
  OrderBook& book = book_for(symbol);
  book.submit_limit(id, side, price, qty, out);
  settle(book, symbol, id, out);
}

void MatchingEngine::submit_market(uint32_t symbol, uint64_t id, Side side, uint32_t qty,
                                   std::vector<Trade>& out) {
  out.clear();
  if (qty == 0 || ids_.contains(id)) return;
  OrderBook* book = find_book(symbol);
  if (book == nullptr) return;  // unknown symbol: no liquidity, and no book is created for it
  book->submit_market(id, side, qty, out);
  settle(*book, symbol, id, out);
}

bool MatchingEngine::cancel(uint64_t id) {
  const auto symbol = ids_.find(id);
  if (!symbol) return false;
  OrderBook* book = find_book(*symbol);
  if (book == nullptr || !book->cancel(id)) return false;  // cannot happen while consistent
  ids_.erase(id);
  return true;
}

bool MatchingEngine::cancel(uint32_t symbol, uint64_t id) {
  const auto owner = ids_.find(id);
  if (!owner || *owner != symbol) return false;
  return cancel(id);
}

bool MatchingEngine::modify(uint64_t id, int64_t new_price, uint32_t new_qty,
                            std::vector<Trade>& out) {
  out.clear();
  const auto symbol = ids_.find(id);
  if (!symbol) return false;
  OrderBook* book = find_book(*symbol);
  if (book == nullptr) return false;  // cannot happen while consistent
  book->modify(id, new_price, new_qty, out);
  settle(*book, *symbol, id, out);
  return true;
}

std::optional<uint32_t> MatchingEngine::symbol_of(uint64_t id) const { return ids_.find(id); }

std::optional<int64_t> MatchingEngine::best_bid(uint32_t symbol) const {
  const OrderBook* b = book(symbol);
  return b ? b->best_bid() : std::nullopt;
}

std::optional<int64_t> MatchingEngine::best_ask(uint32_t symbol) const {
  const OrderBook* b = book(symbol);
  return b ? b->best_ask() : std::nullopt;
}

uint32_t MatchingEngine::total_qty_at(uint32_t symbol, Side side, int64_t price) const {
  const OrderBook* b = book(symbol);
  return b ? b->total_qty_at(side, price) : 0u;
}

bool MatchingEngine::check_invariants() const {
  if (!ids_.check_invariants() || !symbol_slot_.check_invariants()) return false;
  if (symbols_.size() != books_.size() || symbol_slot_.size() != books_.size()) return false;

  std::size_t resting = 0;
  for (std::size_t i = 0; i < books_.size(); ++i) {
    if (!books_[i].check_invariants()) return false;
    if (symbol_slot_.find(symbols_[i]) != i) return false;
    resting += books_[i].resting_orders();
  }
  if (resting != ids_.size()) return false;  // something resting is missing from the id table

  bool consistent = true;  // and every table entry must really be resting on its symbol
  ids_.for_each([&](uint64_t id, uint32_t symbol) {
    const OrderBook* b = book(symbol);
    if (b == nullptr || !b->is_resting(id)) consistent = false;
  });
  return consistent;
}

}  // namespace lob
