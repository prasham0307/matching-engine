// Interactive demo: type commands, watch trades and the book. Not part of the hot path, so
// printing and allocation are fine here.
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lob/naive_matcher.hpp"
#include "lob/order_book.hpp"

namespace {

using lob::Side;
using lob::Trade;
using Trades = std::vector<Trade>;
using Level = std::pair<int64_t, uint32_t>;

// The demo accepts prices in this range only. It keeps the NaiveMatcher display (which scans a
// price range) cheap and rules out nonsense input.
constexpr int64_t kMinPrice = 1;
constexpr int64_t kMaxPrice = 1'000'000;

// Auto-assigned ids continue from here after `scenario quiz`, so the quiz example order is id 20.
constexpr uint64_t kIdAfterQuiz = 20;

template <class T>
std::optional<T> parse_number(std::string_view s) {
  T value{};
  const char* end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, value);
  if (s.empty() || ec != std::errc{} || ptr != end) return std::nullopt;
  return value;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::optional<Side> parse_side(const std::string& s) {
  const std::string l = lower(s);
  if (l == "buy") return Side::Buy;
  if (l == "sell") return Side::Sell;
  return std::nullopt;
}

const char* side_name(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

uint64_t total_qty(const Trades& trades) {
  uint64_t sum = 0;
  for (const Trade& t : trades) sum += t.qty;
  return sum;
}

void print_trades(const Trades& trades) {
  for (const Trade& t : trades) {
    std::cout << "TRADE maker=" << t.makerId << " taker=" << t.takerId << " price=" << t.price
              << " qty=" << t.qty << "\n";
  }
}

void print_help() {
  std::cout <<
      "Commands:\n"
      "  limit buy|sell PRICE QTY    submit a limit order (id is assigned and printed)\n"
      "  market buy|sell QTY         submit a market order (unfilled part is dropped)\n"
      "  cancel ID                   cancel a resting order\n"
      "  modify ID PRICE QTY         cancel and resubmit with the same id (goes to the back\n"
      "                              of the queue); QTY 0 cancels the order\n"
      "  book                        show the book\n"
      "  scenario quiz               reset and load the Session 0 example book\n"
      "  help, quit\n"
      "Prices are integer ticks in [" << kMinPrice << ", " << kMaxPrice << "], quantities are\n"
      "positive integers.\n";
}

template <class M>
class Demo {
 public:
  Demo(std::string engine_name, bool interactive)
      : engine_name_(std::move(engine_name)), interactive_(interactive) {}

  void run() {
    std::cout << "lob_demo using " << engine_name_ << ". Type 'help' for commands.\n";
    std::string line;
    while (true) {
      if (interactive_) std::cout << "> " << std::flush;
      if (!std::getline(std::cin, line)) break;
      if (!interactive_) std::cout << "> " << line << "\n";  // echo so piped sessions read well
      if (!handle(line)) break;
    }
  }

 private:
  // Returns false when the session should end.
  bool handle(const std::string& line) {
    std::istringstream is(line);
    std::vector<std::string> tok;
    for (std::string w; is >> w;) tok.push_back(w);
    if (tok.empty() || tok[0][0] == '#') return true;

    const std::string cmd = lower(tok[0]);
    if (cmd == "quit" || cmd == "exit") return false;
    if (cmd == "help") {
      print_help();
    } else if (cmd == "book") {
      print_book();
    } else if (cmd == "limit") {
      do_limit(tok);
    } else if (cmd == "market") {
      do_market(tok);
    } else if (cmd == "cancel") {
      do_cancel(tok);
    } else if (cmd == "modify") {
      do_modify(tok);
    } else if (cmd == "scenario") {
      do_scenario(tok);
    } else {
      std::cout << "error: unknown command '" << tok[0] << "'. Type 'help'.\n";
    }
    return true;
  }

  // Validates a price; prints the reason and returns nullopt if it is unusable.
  static std::optional<int64_t> read_price(const std::string& s) {
    const auto price = parse_number<int64_t>(s);
    if (!price) {
      std::cout << "error: bad price '" << s << "' (expected an integer number of ticks)\n";
      return std::nullopt;
    }
    if (*price < kMinPrice || *price > kMaxPrice) {
      std::cout << "rejected: price must be between " << kMinPrice << " and " << kMaxPrice
                << "\n";
      return std::nullopt;
    }
    return price;
  }

  static std::optional<uint32_t> read_qty(const std::string& s) {
    const auto qty = parse_number<uint32_t>(s);
    if (!qty) {
      std::cout << "error: bad qty '" << s << "' (expected a non-negative integer)\n";
      return std::nullopt;
    }
    return qty;
  }

  static std::optional<uint64_t> read_id(const std::string& s) {
    const auto id = parse_number<uint64_t>(s);
    if (!id) std::cout << "error: bad id '" << s << "' (expected a non-negative integer)\n";
    return id;
  }

  void remember_price(int64_t price) {
    if (!range_) {
      range_ = std::make_pair(price, price);
    } else {
      range_->first = std::min(range_->first, price);
      range_->second = std::max(range_->second, price);
    }
  }

  void report_fill(uint64_t id, int64_t price, uint32_t qty, const Trades& trades) {
    print_trades(trades);
    const uint64_t left = qty - total_qty(trades);
    if (left > 0) {
      std::cout << "order id=" << id << " rests with " << left << " remaining @ " << price << "\n";
    } else {
      std::cout << "order id=" << id << " fully filled\n";
    }
  }

  void do_limit(const std::vector<std::string>& tok) {
    if (tok.size() != 4) {
      std::cout << "usage: limit buy|sell PRICE QTY\n";
      return;
    }
    const auto side = parse_side(tok[1]);
    if (!side) {
      std::cout << "error: side must be 'buy' or 'sell', got '" << tok[1] << "'\n";
      return;
    }
    const auto price = read_price(tok[2]);
    const auto qty = price ? read_qty(tok[3]) : std::nullopt;
    if (!price || !qty) return;
    if (*qty == 0) {
      std::cout << "rejected: qty must be greater than 0\n";
      return;
    }

    const uint64_t id = next_id_++;
    remember_price(*price);
    std::cout << "order id=" << id << ": LIMIT " << side_name(*side) << " " << *qty << " @ "
              << *price << "\n";
    report_fill(id, *price, *qty, m_.submit_limit(id, *side, *price, *qty));
    print_book();
  }

  void do_market(const std::vector<std::string>& tok) {
    if (tok.size() != 3) {
      std::cout << "usage: market buy|sell QTY\n";
      return;
    }
    const auto side = parse_side(tok[1]);
    if (!side) {
      std::cout << "error: side must be 'buy' or 'sell', got '" << tok[1] << "'\n";
      return;
    }
    const auto qty = read_qty(tok[2]);
    if (!qty) return;
    if (*qty == 0) {
      std::cout << "rejected: qty must be greater than 0\n";
      return;
    }

    const uint64_t id = next_id_++;
    std::cout << "order id=" << id << ": MARKET " << side_name(*side) << " " << *qty << "\n";
    const Trades trades = m_.submit_market(id, *side, *qty);
    print_trades(trades);
    const uint64_t left = *qty - total_qty(trades);
    if (trades.empty()) std::cout << "no liquidity: nothing traded\n";
    if (left > 0) std::cout << left << " unfilled and dropped (market orders never rest)\n";
    print_book();
  }

  void do_cancel(const std::vector<std::string>& tok) {
    if (tok.size() != 2) {
      std::cout << "usage: cancel ID\n";
      return;
    }
    const auto id = read_id(tok[1]);
    if (!id) return;
    if (m_.cancel(*id)) {
      std::cout << "order id=" << *id << " cancelled\n";
      print_book();
    } else {
      std::cout << "rejected: no resting order with id " << *id
                << " (unknown, or already filled or cancelled)\n";
    }
  }

  void do_modify(const std::vector<std::string>& tok) {
    if (tok.size() != 4) {
      std::cout << "usage: modify ID PRICE QTY\n";
      return;
    }
    const auto id = read_id(tok[1]);
    const auto price = id ? read_price(tok[2]) : std::nullopt;
    const auto qty = price ? read_qty(tok[3]) : std::nullopt;
    if (!id || !price || !qty) return;

    if (!m_.is_resting(*id)) {
      std::cout << "rejected: no resting order with id " << *id
                << " (unknown, or already filled or cancelled)\n";
      return;
    }
    if (*qty == 0) {
      m_.modify(*id, *price, 0);
      std::cout << "order id=" << *id << " cancelled (modify to qty 0 cancels)\n";
      print_book();
      return;
    }

    remember_price(*price);
    std::cout << "order id=" << *id << ": MODIFY to " << *qty << " @ " << *price
              << " (new time priority)\n";
    report_fill(*id, *price, *qty, m_.modify(*id, *price, *qty));
    print_book();
  }

  void do_scenario(const std::vector<std::string>& tok) {
    if (tok.size() != 2 || lower(tok[1]) != "quiz") {
      std::cout << "usage: scenario quiz\n";
      return;
    }
    m_ = M{};
    range_.reset();
    // asks: 103: id3 x4 | 102: id2 x6 | 101: id1 x3 then id4 x2
    // bids: 100: id11 x5 then id12 x3 | 99: id13 x10
    m_.submit_limit(3, Side::Sell, 103, 4);
    m_.submit_limit(2, Side::Sell, 102, 6);
    m_.submit_limit(1, Side::Sell, 101, 3);
    m_.submit_limit(4, Side::Sell, 101, 2);
    m_.submit_limit(11, Side::Buy, 100, 5);
    m_.submit_limit(12, Side::Buy, 100, 3);
    m_.submit_limit(13, Side::Buy, 99, 10);
    remember_price(99);
    remember_price(103);
    next_id_ = kIdAfterQuiz;
    std::cout << "scenario quiz loaded (ids 1-4 asks, 11-13 bids). Next order id is "
              << next_id_ << ".\n";
    print_book();
  }

  // (price, qty) per level, best price first. OrderBook reports it directly; NaiveMatcher has
  // no depth, so scan every price the demo has ever seen (the only prices that can hold orders).
  std::vector<Level> levels(Side side) const {
    if constexpr (requires { m_.depth(side); }) {
      return m_.depth(side);
    } else {
      std::vector<Level> out;
      if (!range_) return out;
      const auto [lo, hi] = *range_;
      if (side == Side::Buy) {
        for (int64_t p = hi; p >= lo; --p) {
          if (const uint32_t q = m_.total_qty_at(side, p); q > 0) out.emplace_back(p, q);
        }
      } else {
        for (int64_t p = lo; p <= hi; ++p) {
          if (const uint32_t q = m_.total_qty_at(side, p); q > 0) out.emplace_back(p, q);
        }
      }
      return out;
    }
  }

  static void print_level(const Level& level) {
    std::cout << "    " << std::setw(8) << level.first << "  x " << std::setw(6) << level.second
              << "\n";
  }

  void print_book() const {
    const std::vector<Level> asks = levels(Side::Sell);  // lowest first
    const std::vector<Level> bids = levels(Side::Buy);   // highest first

    std::cout << "\n  ASKS (sell)\n";
    if (asks.empty()) std::cout << "      (empty)\n";
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) print_level(*it);  // highest at top

    if (!asks.empty() && !bids.empty()) {
      std::cout << "  ---- spread " << asks.front().first - bids.front().first << " (bid "
                << bids.front().first << ", ask " << asks.front().first << ") ----\n";
    } else {
      std::cout << "  ---- spread n/a ----\n";
    }

    for (const Level& level : bids) print_level(level);
    if (bids.empty()) std::cout << "      (empty)\n";
    std::cout << "  BIDS (buy)\n\n";
  }

  std::string engine_name_;
  bool interactive_;
  M m_;
  uint64_t next_id_ = 1;
  std::optional<std::pair<int64_t, int64_t>> range_;  // lowest and highest price ever used
};

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " [--naive]\n"
            << "  Reads commands from stdin. --naive runs NaiveMatcher instead of OrderBook.\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool naive = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--naive") {
      naive = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "unknown argument '" << arg << "'\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  const bool interactive = isatty(STDIN_FILENO) != 0;
  if (naive) {
    Demo<lob::NaiveMatcher>("NaiveMatcher", interactive).run();
  } else {
    Demo<lob::OrderBook>("OrderBook", interactive).run();
  }
  return 0;
}
