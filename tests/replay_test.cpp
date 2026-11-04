#include "lob/replay.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_util.hpp"

#ifndef LOB_TEST_DATA_DIR
#error "LOB_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

using lob::Side;
using lob::Trade;
using namespace lob::replay;

std::string read_file(const std::string& name) {
  std::ifstream in(std::string(LOB_TEST_DATA_DIR) + "/" + name, std::ios::binary);
  EXPECT_TRUE(in) << "cannot open test data file " << name;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

uint64_t fnv(const std::string& text) {
  Fnv1a64 h;
  h.add(text);
  return h.value();
}

ReplayResult run(const std::string& input, bool verify, std::string* trade_log = nullptr) {
  std::istringstream in(input);
  std::ostringstream log;
  ReplayOptions options;
  options.verify = verify;
  options.trade_log = trade_log != nullptr ? &log : nullptr;
  const ReplayResult r = replay(in, options);
  if (trade_log != nullptr) *trade_log = log.str();
  return r;
}

// ---- parsing ----

TEST(ReplayParse, EachEventKind) {
  Event e;
  std::string err;

  ASSERT_EQ(parse_line("LIMIT,5,10,BUY,101,7", e, err), ParseStatus::Event);
  EXPECT_EQ(e.kind, EventKind::Limit);
  EXPECT_EQ(e.id, 5u);
  EXPECT_EQ(e.symbol, 10u);
  EXPECT_EQ(e.side, Side::Buy);
  EXPECT_EQ(e.price, 101);
  EXPECT_EQ(e.qty, 7u);

  ASSERT_EQ(parse_line("MARKET,6,20,SELL,9", e, err), ParseStatus::Event);
  EXPECT_EQ(e.kind, EventKind::Market);
  EXPECT_EQ(e.id, 6u);
  EXPECT_EQ(e.symbol, 20u);
  EXPECT_EQ(e.side, Side::Sell);
  EXPECT_EQ(e.qty, 9u);

  ASSERT_EQ(parse_line("CANCEL,7", e, err), ParseStatus::Event);
  EXPECT_EQ(e.kind, EventKind::Cancel);
  EXPECT_EQ(e.id, 7u);

  ASSERT_EQ(parse_line("MODIFY,8,-3,12", e, err), ParseStatus::Event);
  EXPECT_EQ(e.kind, EventKind::Modify);
  EXPECT_EQ(e.id, 8u);
  EXPECT_EQ(e.price, -3);  // any int64 is a legal price
  EXPECT_EQ(e.qty, 12u);
}

TEST(ReplayParse, CommentsBlankLinesWhitespaceAndCarriageReturns) {
  Event e;
  std::string err;
  EXPECT_EQ(parse_line("", e, err), ParseStatus::Skip);
  EXPECT_EQ(parse_line("   \t ", e, err), ParseStatus::Skip);
  EXPECT_EQ(parse_line("# a comment, with, commas", e, err), ParseStatus::Skip);
  EXPECT_EQ(parse_line("   # indented comment", e, err), ParseStatus::Skip);
  ASSERT_EQ(parse_line("  LIMIT , 5 ,10, SELL ,101 , 7 \r", e, err), ParseStatus::Event);
  EXPECT_EQ(e.id, 5u);
  EXPECT_EQ(e.side, Side::Sell);
  EXPECT_EQ(e.qty, 7u);
}

TEST(ReplayParse, ExtremeValuesAreAccepted) {
  Event e;
  std::string err;
  ASSERT_EQ(parse_line("LIMIT,18446744073709551615,4294967295,BUY,-9223372036854775808,4294967295",
                       e, err),
            ParseStatus::Event);
  EXPECT_EQ(e.id, UINT64_MAX);
  EXPECT_EQ(e.symbol, UINT32_MAX);
  EXPECT_EQ(e.price, INT64_MIN);
  EXPECT_EQ(e.qty, UINT32_MAX);
}

TEST(ReplayParse, MalformedLinesAreRejectedWithAClearMessage) {
  struct Case {
    const char* line;
    const char* message_contains;
  };
  const Case cases[] = {
      {"HOLD,1", "unknown event type 'HOLD'"},
      {"limit,1,10,BUY,100,5", "unknown event type 'limit'"},  // keywords are exact uppercase
      {"LIMIT,1,10,BUY,100", "LIMIT needs 6 fields"},
      {"LIMIT,1,10,BUY,100,5,9", "LIMIT needs 6 fields"},
      {"LIMIT,1,10,BUY,100,5,9,9,9,9", "but the line has 10"},
      {"MARKET,1,10,BUY", "MARKET needs 5 fields"},
      {"CANCEL", "CANCEL needs 2 fields"},
      {"CANCEL,1,2", "CANCEL needs 2 fields"},
      {"MODIFY,1,100", "MODIFY needs 4 fields"},
      {"LIMIT,x,10,BUY,100,5", "id must be"},
      {"LIMIT,-1,10,BUY,100,5", "id must be"},
      {"LIMIT,18446744073709551616,10,BUY,100,5", "id must be"},
      {"LIMIT,1,y,BUY,100,5", "symbol must be"},
      {"LIMIT,1,4294967296,BUY,100,5", "symbol must be"},
      {"LIMIT,1,10,HOLD,100,5", "side must be BUY or SELL, got 'HOLD'"},
      {"LIMIT,1,10,buy,100,5", "side must be BUY or SELL"},
      {"LIMIT,1,10,BUY,1.5,5", "price must be"},
      {"LIMIT,1,10,BUY,,5", "price must be"},
      {"LIMIT,1,10,BUY,100,-5", "qty must be"},
      {"LIMIT,1,10,BUY,100,4294967296", "qty must be"},
      {"MARKET,1,10,BUY,five", "qty must be"},
      {"MODIFY,1,abc,5", "new price must be"},
      {"MODIFY,1,100,-1", "new qty must be"},
  };
  for (const Case& c : cases) {
    Event e;
    std::string err;
    EXPECT_EQ(parse_line(c.line, e, err), ParseStatus::Error) << c.line;
    EXPECT_NE(err.find(c.message_contains), std::string::npos)
        << "line '" << c.line << "' gave message '" << err << "', expected it to contain '"
        << c.message_contains << "'";
  }
}

TEST(Replay, MalformedLineStopsTheRunAtThatLineNumber) {
  const std::string input =
      "# header comment\n"
      "LIMIT,1,10,BUY,100,5\n"
      "\n"
      "LIMIT,2,10,BUY,abc,5\n"  // line 4
      "LIMIT,3,10,BUY,100,5\n";
  const ReplayResult r = run(input, false);
  EXPECT_EQ(r.failure, Failure::Malformed);
  EXPECT_EQ(r.line, 4u);
  EXPECT_NE(r.message.find("price must be"), std::string::npos);
  EXPECT_EQ(r.events, 1u);  // only the first event ran
}

// ---- golden file ----

TEST(Replay, GoldenFileMatchesTheHandCheckedTradeLog) {
  const std::string events = read_file("golden_events.csv");
  const std::string expected = read_file("golden_trades.csv");
  ASSERT_FALSE(events.empty());
  ASSERT_FALSE(expected.empty());

  std::string log;
  const ReplayResult r = run(events, true, &log);  // verified against NaiveMatcher as well
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(log, expected);
  EXPECT_EQ(r.trade_hash, fnv(expected));
  EXPECT_EQ(r.trades, 6u);
  EXPECT_EQ(r.events, 19u);
  EXPECT_EQ(r.rejected_orders, 2u);   // id 3 reused while resting, and the quantity-0 order
  EXPECT_EQ(r.missed_cancels, 1u);    // the second CANCEL,3
  EXPECT_EQ(r.missed_modifies, 1u);   // MODIFY,99

  ASSERT_EQ(r.symbols.size(), 3u);  // 10, 20, 30 in first-seen order; 40 never gets a book
  EXPECT_EQ(r.symbols[0].symbol, 10u);
  EXPECT_EQ(r.symbols[0].best_bid, std::nullopt);
  EXPECT_EQ(r.symbols[0].best_ask, 50);
  EXPECT_EQ(r.symbols[0].resting, 1u);
  EXPECT_EQ(r.symbols[1].symbol, 20u);
  EXPECT_EQ(r.symbols[1].best_bid, std::nullopt);
  EXPECT_EQ(r.symbols[1].best_ask, 100);
  EXPECT_EQ(r.symbols[2].symbol, 30u);
  EXPECT_EQ(r.symbols[2].best_bid, 50);
  EXPECT_EQ(r.symbols[2].best_ask, std::nullopt);
}

TEST(Replay, TradeLogHasOneLinePerTradeInTheDocumentedFormat) {
  char buf[96];
  const std::size_t n = format_trade(buf, Trade{4, 9, 101, 5}, 10);
  EXPECT_EQ(std::string(buf, n), "4,9,10,101,5");
  const std::size_t m = format_trade(buf, Trade{UINT64_MAX, UINT64_MAX, INT64_MIN, UINT32_MAX}, UINT32_MAX);
  EXPECT_LT(m, 96u);  // the longest possible line fits the buffer
}

TEST(Replay, Fnv1aMatchesKnownVectors) {
  Fnv1a64 empty;
  EXPECT_EQ(empty.value(), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(fnv("foobar"), 0x85944171f73967e8ULL);
}

// ---- determinism and generation ----

std::string generated(uint64_t events, uint32_t symbols, uint64_t seed) {
  std::ostringstream out;
  generate_events(out, events, symbols, seed);
  return out.str();
}

TEST(Replay, GeneratorIsDeterministicAndRespectsItsArguments) {
  EXPECT_EQ(generated(5000, 3, 1), generated(5000, 3, 1));
  EXPECT_NE(generated(5000, 3, 1), generated(5000, 3, 2));
  EXPECT_NE(generated(5000, 3, 1), generated(5000, 4, 1));

  // Every line parses, there are exactly `events` events, and symbols stay within 1..3.
  const std::string text = generated(5000, 3, 1);
  std::istringstream in(text);
  std::string line;
  uint64_t events = 0;
  bool symbols_ok = true;
  while (std::getline(in, line)) {
    Event e;
    std::string err;
    const ParseStatus st = parse_line(line, e, err);
    ASSERT_NE(st, ParseStatus::Error) << line << ": " << err;
    if (st != ParseStatus::Event) continue;
    ++events;
    if ((e.kind == EventKind::Limit || e.kind == EventKind::Market) && (e.symbol < 1 || e.symbol > 3)) {
      symbols_ok = false;
    }
  }
  EXPECT_EQ(events, 5000u);
  EXPECT_TRUE(symbols_ok);
}

TEST(Replay, ReplayingTheSameInputTwiceGivesTheSameHash) {
  const std::string input = generated(30000, 3, 7);
  std::string log_a, log_b;
  const ReplayResult a = run(input, false, &log_a);
  const ReplayResult b = run(input, false, &log_b);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a.trade_hash, b.trade_hash);
  EXPECT_EQ(a.trades, b.trades);
  EXPECT_EQ(log_a, log_b);
  EXPECT_GT(a.trades, 1000u);  // the stream really trades, so the hash means something
  EXPECT_EQ(a.trade_hash, fnv(log_a));  // the hash is the hash of the log text

  // A different stream gives a different hash.
  EXPECT_NE(run(generated(30000, 3, 8), false).trade_hash, a.trade_hash);
}

TEST(Replay, VerifyPassesOnAGeneratedStreamAndDoesNotChangeTheResult) {
  const std::string input = generated(20000, 3, 11);
  const ReplayResult plain = run(input, false);
  const ReplayResult verified = run(input, true);
  ASSERT_TRUE(verified.ok()) << verified.message;
  EXPECT_EQ(plain.trade_hash, verified.trade_hash);
  EXPECT_EQ(plain.trades, verified.trades);
}

// ---- the oracle and the comparison used by --verify ----

TEST(ReplayOracle, AppliesTheSameRulesAsTheEngine) {
  Oracle o;
  const auto limit = [&](uint64_t id, uint32_t sym, Side side, int64_t price, uint32_t qty) {
    Event e;
    e.kind = EventKind::Limit;
    e.id = id;
    e.symbol = sym;
    e.side = side;
    e.price = price;
    e.qty = qty;
    return o.apply(e);
  };
  EXPECT_TRUE(limit(1, 10, Side::Buy, 100, 5).trades.empty());
  EXPECT_TRUE(limit(2, 20, Side::Sell, 100, 5).trades.empty());  // other symbol: no trade
  EXPECT_TRUE(limit(1, 20, Side::Sell, 99, 5).trades.empty());   // id 1 is taken on symbol 10
  Event cancel;
  cancel.kind = EventKind::Cancel;
  cancel.id = 1;
  EXPECT_TRUE(o.apply(cancel).found);
  EXPECT_FALSE(o.apply(cancel).found);
  EXPECT_EQ(limit(1, 20, Side::Buy, 100, 2).trades, (std::vector<Trade>{{2, 1, 100, 2}}));  // id free again
}

TEST(ReplayOracle, DiffReportsEveryKindOfDisagreement) {
  Oracle::Outcome oracle;
  oracle.trades = {{1, 2, 100, 5}};
  oracle.found = true;

  EXPECT_FALSE(diff_outcomes({{1, 2, 100, 5}}, true, oracle).has_value());
  EXPECT_TRUE(diff_outcomes({}, true, oracle).has_value());                    // a missing trade
  EXPECT_TRUE(diff_outcomes({{1, 2, 100, 5}, {3, 2, 100, 1}}, true, oracle));  // an extra trade
  EXPECT_TRUE(diff_outcomes({{1, 2, 101, 5}}, true, oracle));                  // wrong price
  EXPECT_TRUE(diff_outcomes({{1, 2, 100, 4}}, true, oracle));                  // wrong quantity
  EXPECT_TRUE(diff_outcomes({{9, 2, 100, 5}}, true, oracle));                  // wrong maker
  EXPECT_TRUE(diff_outcomes({{1, 2, 100, 5}}, false, oracle));                 // wrong found flag
  const auto text = diff_outcomes({{1, 2, 101, 5}}, true, oracle);
  ASSERT_TRUE(text.has_value());
  EXPECT_NE(text->find("engine"), std::string::npos);
  EXPECT_NE(text->find("NaiveMatcher"), std::string::npos);
}

TEST(Replay, RejectedAndMissedEventsAreCountedNotTreatedAsErrors) {
  const std::string input =
      "LIMIT,1,10,BUY,100,5\n"
      "LIMIT,1,10,BUY,100,5\n"   // duplicate id
      "LIMIT,2,10,BUY,100,0\n"   // zero quantity
      "MARKET,3,10,BUY,0\n"      // zero quantity
      "CANCEL,77\n"
      "MODIFY,88,100,1\n";
  const ReplayResult r = run(input, true);
  ASSERT_TRUE(r.ok()) << r.message;
  EXPECT_EQ(r.events, 6u);
  EXPECT_EQ(r.rejected_orders, 3u);
  EXPECT_EQ(r.missed_cancels, 1u);
  EXPECT_EQ(r.missed_modifies, 1u);
  EXPECT_EQ(r.trades, 0u);
}

}  // namespace
