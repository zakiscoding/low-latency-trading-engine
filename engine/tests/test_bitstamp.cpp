#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lle/bitstamp.hpp"
#include "lle/ladder.hpp"

using namespace lle;

namespace {
struct Book : coinbase::NullHandler {
  BidLadder bids{14};
  AskLadder asks{14};
  std::vector<coinbase::TradePrint> trades;
  void on_l2_begin(std::string_view, bool snap) {
    if (snap) { bids.clear(); asks.clear(); }
  }
  void on_l2(const coinbase::L2Update& u) { if (u.bid) bids.set(u.price, u.qty); else asks.set(u.price, u.qty); }
  void on_trade(std::string_view, const coinbase::TradePrint& t) { trades.push_back(t); }
};
std::string book_msg(const char* chan, const char* us, const char* bids, const char* asks) {
  return std::string(R"({"data":{"timestamp":"1","microtimestamp":")") + us + R"(","bids":)" + bids +
         R"(,"asks":)" + asks + R"(},"channel":")" + chan + R"(","event":"data"})";
}
bool feed(Book& b, bitstamp::State& s, coinbase::Stats& st, const std::string& m) {
  return bitstamp::parse(m.data(), m.size(), b, st, s);
}
}  // namespace

TEST(Bitstamp, SnapshotThenDiffs) {
  Book b;
  bitstamp::State s;
  coinbase::Stats st;
  // Diff before any snapshot: ignored.
  EXPECT_TRUE(feed(b, s, st, book_msg("diff_order_book_btcusd", "90", R"([["100.00","1.00000000"]])", "[]")));
  EXPECT_TRUE(b.bids.empty());
  EXPECT_TRUE(feed(b, s, st, book_msg("order_book_btcusd", "100", R"([["100.00","2.00000000"],["99.50","1"]])",
                                      R"([["100.10","3.5"]])")));
  EXPECT_EQ(b.bids.best_price(), 10000);
  EXPECT_EQ(b.bids.best_qty(), 200000000);
  EXPECT_EQ(b.asks.best_price(), 10010);
  // Stale diff (older than the snapshot): ignored. Newer: applied, 0 removes.
  EXPECT_TRUE(feed(b, s, st, book_msg("diff_order_book_btcusd", "99", R"([["100.00","0.00000000"]])", "[]")));
  EXPECT_EQ(b.bids.best_price(), 10000);
  EXPECT_TRUE(feed(b, s, st, book_msg("diff_order_book_btcusd", "101", R"([["100.00","0.00000000"]])", "[]")));
  EXPECT_EQ(b.bids.best_price(), 9950);
  EXPECT_EQ(st.malformed, 0u);
}

// The live bug: a level the diffs never removed (diff lost before seeding) must be
// gone after the next snapshot.
TEST(Bitstamp, NextSnapshotHealsStaleLevel) {
  Book b;
  bitstamp::State s;
  coinbase::Stats st;
  feed(b, s, st, book_msg("order_book_btcusd", "100", R"([["670.00","1"]])", R"([["670.10","1"]])"));
  feed(b, s, st, book_msg("diff_order_book_btcusd", "150", "[]", R"([["669.00","1"],["670.10","0"]])"));
  EXPECT_EQ(b.bids.best_price(), 67000);
  EXPECT_EQ(b.asks.best_price(), 66900);  // crossed: the bid removal was missed
  feed(b, s, st, book_msg("order_book_btcusd", "200", R"([["668.90","1"]])", R"([["669.00","1"]])"));
  EXPECT_EQ(b.bids.best_price(), 66890);
  EXPECT_EQ(b.asks.best_price(), 66900);
  // Older snapshot arriving late is ignored.
  feed(b, s, st, book_msg("order_book_btcusd", "180", R"([["670.00","1"]])", R"([["670.10","1"]])"));
  EXPECT_EQ(b.bids.best_price(), 66890);
}

TEST(Bitstamp, TradesAndControlMessages) {
  Book b;
  bitstamp::State s;
  coinbase::Stats st;
  const std::string sub = R"({"event":"bts:subscription_succeeded","channel":"live_trades_btcusd","data":{}})";
  EXPECT_TRUE(feed(b, s, st, sub));
  const std::string tr =
      R"({"data":{"id":1,"timestamp":"1","amount":0.0125,"amount_str":"0.01250000","price":67100,"price_str":"67100.25","type":1,"microtimestamp":"5","buy_order_id":2,"sell_order_id":3},"channel":"live_trades_btcusd","event":"trade"})";
  EXPECT_TRUE(feed(b, s, st, tr));
  ASSERT_EQ(b.trades.size(), 1u);
  EXPECT_EQ(b.trades[0].price, 6710025);
  EXPECT_EQ(b.trades[0].qty, 1250000);
  EXPECT_FALSE(b.trades[0].buy);
}

TEST(Bitstamp, TruncationsAreSafe) {
  Book b;
  bitstamp::State s;
  coinbase::Stats st;
  const std::string m = book_msg("order_book_btcusd", "100", R"([["100.00","2.00000000"],["99.50","1"]])",
                                 R"([["100.10","3.5"]])");
  for (std::size_t n = 0; n < m.size(); ++n) {
    std::vector<char> buf(m.begin(), m.begin() + static_cast<long>(n));
    bitstamp::State s2;
    bitstamp::parse(buf.data(), buf.size(), b, st, s2);
  }
}
