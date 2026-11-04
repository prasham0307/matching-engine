#include "lob/replay.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <sstream>
#include <system_error>

#include "lob/order_generator.hpp"

namespace lob::replay {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

template <class T>
bool parse_number(std::string_view s, T& out) {
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return !s.empty() && ec == std::errc{} && ptr == end;
}

std::string quoted(std::string_view s) { return "'" + std::string(s) + "'"; }

// "id must be ..., got 'x'"
std::string bad(std::string_view field, std::string_view expected, std::string_view got) {
  return std::string(field) + " must be " + std::string(expected) + ", got " + quoted(got);
}

const char* side_text(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

std::string describe_trades(const std::vector<Trade>& trades) {
  std::ostringstream os;
  os << "[";
  for (std::size_t i = 0; i < trades.size(); ++i) {
    if (i != 0) os << ", ";
    os << "(maker " << trades[i].makerId << ", taker " << trades[i].takerId << ", price "
       << trades[i].price << ", qty " << trades[i].qty << ")";
  }
  os << "]";
  return os.str();
}

bool same_trades(const std::vector<Trade>& a, const std::vector<Trade>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].makerId != b[i].makerId || a[i].takerId != b[i].takerId || a[i].price != b[i].price ||
        a[i].qty != b[i].qty) {
      return false;
    }
  }
  return true;
}

}  // namespace

ParseStatus parse_line(std::string_view line, Event& event, std::string& error) {
  const std::string_view v = trim(line);
  if (v.empty() || v.front() == '#') return ParseStatus::Skip;

  // Split on commas into at most 7 fields; more than 6 is an error either way.
  std::array<std::string_view, 7> f{};
  std::size_t n = 0;
  std::string_view rest = v;
  for (;;) {
    const std::size_t comma = rest.find(',');
    if (n < f.size()) f[n] = trim(rest.substr(0, comma));
    ++n;
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }

  const std::string_view kind = f[0];
  std::size_t want = 0;
  const char* shape = "";
  if (kind == "LIMIT") {
    want = 6;
    shape = "LIMIT,<id>,<symbol>,<BUY|SELL>,<price_ticks>,<qty>";
    event.kind = EventKind::Limit;
  } else if (kind == "MARKET") {
    want = 5;
    shape = "MARKET,<id>,<symbol>,<BUY|SELL>,<qty>";
    event.kind = EventKind::Market;
  } else if (kind == "CANCEL") {
    want = 2;
    shape = "CANCEL,<id>";
    event.kind = EventKind::Cancel;
  } else if (kind == "MODIFY") {
    want = 4;
    shape = "MODIFY,<id>,<new_price_ticks>,<new_qty>";
    event.kind = EventKind::Modify;
  } else {
    error = "unknown event type " + quoted(kind) + " (expected LIMIT, MARKET, CANCEL or MODIFY)";
    return ParseStatus::Error;
  }
  if (n != want) {
    error = std::string(kind) + " needs " + std::to_string(want) + " fields (" + shape +
            ") but the line has " + std::to_string(n);
    return ParseStatus::Error;
  }

  const auto side_of = [&](std::string_view s, Side& out) {
    if (s == "BUY") {
      out = Side::Buy;
    } else if (s == "SELL") {
      out = Side::Sell;
    } else {
      error = bad("side", "BUY or SELL", s);
      return false;
    }
    return true;
  };
  const auto id_of = [&](std::string_view s) {
    if (!parse_number(s, event.id)) {
      error = bad("id", "a non-negative integer that fits in 64 bits", s);
      return false;
    }
    return true;
  };
  const auto price_of = [&](std::string_view field, std::string_view s) {
    if (!parse_number(s, event.price)) {
      error = bad(field, "an integer number of ticks that fits in 64 bits", s);
      return false;
    }
    return true;
  };
  const auto qty_of = [&](std::string_view field, std::string_view s) {
    if (!parse_number(s, event.qty)) {
      error = bad(field, "an integer from 0 to 4294967295", s);
      return false;
    }
    return true;
  };
  const auto symbol_of = [&](std::string_view s) {
    if (!parse_number(s, event.symbol)) {
      error = bad("symbol", "an integer from 0 to 4294967295", s);
      return false;
    }
    return true;
  };

  event.side = Side::Buy;
  event.price = 0;
  event.qty = 0;
  event.symbol = 0;
  switch (event.kind) {
    case EventKind::Limit:
      if (!id_of(f[1]) || !symbol_of(f[2]) || !side_of(f[3], event.side) ||
          !price_of("price", f[4]) || !qty_of("qty", f[5])) {
        return ParseStatus::Error;
      }
      break;
    case EventKind::Market:
      if (!id_of(f[1]) || !symbol_of(f[2]) || !side_of(f[3], event.side) || !qty_of("qty", f[4])) {
        return ParseStatus::Error;
      }
      break;
    case EventKind::Cancel:
      if (!id_of(f[1])) return ParseStatus::Error;
      break;
    case EventKind::Modify:
      if (!id_of(f[1]) || !price_of("new price", f[2]) || !qty_of("new qty", f[3])) {
        return ParseStatus::Error;
      }
      break;
  }
  return ParseStatus::Event;
}

void generate_events(std::ostream& out, uint64_t events, uint32_t symbols, uint64_t seed) {
  GenConfig cfg;
  cfg.seed = seed;
  OrderGenerator gen(cfg);
  std::mt19937_64 symbol_rng(seed ^ 0x9E3779B97F4A7C15ULL);  // raw output + modulo: portable

  std::string buf = "# lob_replay generated: events=" + std::to_string(events) +
                    " symbols=" + std::to_string(symbols) + " seed=" + std::to_string(seed) + "\n";
  const auto flush = [&] {
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.clear();
  };
  for (uint64_t i = 0; i < events; ++i) {
    const Op op = gen.next();
    const uint32_t symbol = 1 + static_cast<uint32_t>(symbol_rng() % symbols);
    switch (op.kind) {
      case OpKind::Limit:
        buf += "LIMIT," + std::to_string(op.id) + "," + std::to_string(symbol) + "," +
               side_text(op.side) + "," + std::to_string(op.price) + "," + std::to_string(op.qty);
        break;
      case OpKind::Market:
        buf += "MARKET," + std::to_string(op.id) + "," + std::to_string(symbol) + "," +
               side_text(op.side) + "," + std::to_string(op.qty);
        break;
      case OpKind::Cancel:
        buf += "CANCEL," + std::to_string(op.id);
        break;
      case OpKind::Modify:
        buf += "MODIFY," + std::to_string(op.id) + "," + std::to_string(op.price) + "," +
               std::to_string(op.qty);
        break;
    }
    buf += '\n';
    if (buf.size() >= (1u << 16)) flush();
  }
  flush();
}

std::optional<uint32_t> Oracle::owner_of(uint64_t id) const {
  for (const auto& [symbol, book] : books_) {
    if (book.is_resting(id)) return symbol;
  }
  return std::nullopt;
}

Oracle::Outcome Oracle::apply(const Event& e) {
  Outcome out;
  switch (e.kind) {
    case EventKind::Limit:
    case EventKind::Market: {
      // An id larger than every id submitted so far cannot be resting, so skip the scan for it.
      const bool maybe_resting = any_submitted_ && e.id <= max_submitted_;
      const bool taken = maybe_resting && owner_of(e.id).has_value();
      max_submitted_ = any_submitted_ ? std::max(max_submitted_, e.id) : e.id;
      any_submitted_ = true;
      if (taken) break;  // duplicate of a resting id on some symbol: rejected, no trades
      if (e.kind == EventKind::Limit) {
        out.trades = books_[e.symbol].submit_limit(e.id, e.side, e.price, e.qty);
      } else {
        const auto it = books_.find(e.symbol);
        if (it != books_.end()) out.trades = it->second.submit_market(e.id, e.side, e.qty);
      }
      break;
    }
    case EventKind::Cancel: {
      const auto owner = owner_of(e.id);
      out.found = owner && books_[*owner].cancel(e.id);
      break;
    }
    case EventKind::Modify: {
      const auto owner = owner_of(e.id);
      out.found = owner.has_value();
      if (owner) out.trades = books_[*owner].modify(e.id, e.price, e.qty);
      break;
    }
  }
  return out;
}

std::optional<std::string> diff_outcomes(const std::vector<Trade>& engine_trades, bool engine_found,
                                         const Oracle::Outcome& oracle) {
  if (same_trades(engine_trades, oracle.trades) && engine_found == oracle.found) return std::nullopt;
  std::ostringstream os;
  os << "engine:       found=" << (engine_found ? "yes" : "no") << " trades="
     << describe_trades(engine_trades) << "\n  NaiveMatcher: found=" << (oracle.found ? "yes" : "no")
     << " trades=" << describe_trades(oracle.trades);
  return os.str();
}

std::size_t format_trade(char* buf, const Trade& t, uint32_t symbol) {
  char* p = buf;
  char* const end = buf + 96;
  const auto put_number = [&](auto value) {
    p = std::to_chars(p, end, value).ptr;
  };
  put_number(t.makerId);
  *p++ = ',';
  put_number(t.takerId);
  *p++ = ',';
  put_number(symbol);
  *p++ = ',';
  put_number(t.price);
  *p++ = ',';
  put_number(t.qty);
  return static_cast<std::size_t>(p - buf);
}

ReplayResult replay(std::istream& in, const ReplayOptions& options) {
  ReplayResult result;
  MatchingEngine engine(options.engine);
  std::optional<Oracle> oracle;
  if (options.verify) oracle.emplace();

  std::vector<Trade> trades;
  Fnv1a64 hash;
  std::string line;
  std::string error;
  std::string log;  // pending trade log text
  char text[96];
  Event ev;

  const auto flush_log = [&] {
    if (options.trade_log != nullptr && !log.empty()) {
      options.trade_log->write(log.data(), static_cast<std::streamsize>(log.size()));
    }
    log.clear();
  };
  const auto fail = [&](Failure kind, std::string message) {
    flush_log();
    result.failure = kind;
    result.line = result.lines;
    result.message = std::move(message);
    result.trade_hash = hash.value();
    result.id_table_resizes = engine.id_table_resizes();
    return result;
  };

  while (std::getline(in, line)) {
    ++result.lines;
    const ParseStatus status = parse_line(line, ev, error);
    if (status == ParseStatus::Skip) continue;
    if (status == ParseStatus::Error) return fail(Failure::Malformed, error);
    ++result.events;

    uint32_t trade_symbol = ev.symbol;
    bool found = true;
    switch (ev.kind) {
      case EventKind::Limit:
        if (ev.qty == 0 || engine.symbol_of(ev.id)) ++result.rejected_orders;
        engine.submit_limit(ev.symbol, ev.id, ev.side, ev.price, ev.qty, trades);
        break;
      case EventKind::Market:
        if (ev.qty == 0 || engine.symbol_of(ev.id)) ++result.rejected_orders;
        engine.submit_market(ev.symbol, ev.id, ev.side, ev.qty, trades);
        break;
      case EventKind::Cancel:
        trades.clear();
        found = engine.cancel(ev.id);
        if (!found) ++result.missed_cancels;
        break;
      case EventKind::Modify: {
        const auto symbol = engine.symbol_of(ev.id);
        found = engine.modify(ev.id, ev.price, ev.qty, trades);
        if (!found) {
          ++result.missed_modifies;
        } else if (symbol) {
          trade_symbol = *symbol;
        }
        break;
      }
    }

    if (oracle) {
      const Oracle::Outcome expected = oracle->apply(ev);
      if (const auto difference = diff_outcomes(trades, found, expected)) {
        return fail(Failure::Mismatch, "engine and NaiveMatcher disagree\n  event: " +
                                           std::string(trim(line)) + "\n  " + *difference);
      }
    }

    for (const Trade& t : trades) {
      const std::size_t n = format_trade(text, t, trade_symbol);
      text[n] = '\n';
      hash.add(text, n + 1);
      if (options.trade_log != nullptr) log.append(text, n + 1);
      ++result.trades;
    }
    if (log.size() >= (1u << 16)) flush_log();
  }

  flush_log();
  if (options.trade_log != nullptr && !options.trade_log->good()) {
    result.failure = Failure::Io;
    result.line = result.lines;
    result.message = "could not write the trade log";
  }
  result.trade_hash = hash.value();
  result.id_table_resizes = engine.id_table_resizes();
  for (const uint32_t symbol : engine.symbols()) {
    const OrderBook* book = engine.book(symbol);
    result.symbols.push_back(
        SymbolSummary{symbol, book->best_bid(), book->best_ask(), book->resting_orders()});
  }
  return result;
}

}  // namespace lob::replay
