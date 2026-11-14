#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lle/coinbase.hpp"

using namespace lle;
using namespace lle::coinbase;

namespace {
struct Rec : NullHandler {
  std::vector<L2Update> ups;
  std::vector<TradePrint> trades;
  std::vector<std::string> products;
  int snaps = 0, ends = 0, hb = 0;
  std::int64_t seq = -1;
  void on_l2_begin(std::string_view p, bool s) { products.emplace_back(p); snaps += s; }
  void on_l2(const L2Update& u) { ups.push_back(u); }
  void on_l2_end() { ends++; }
  void on_trade(std::string_view, const TradePrint& t) { trades.push_back(t); }
  void on_heartbeat() { hb++; }
  void on_sequence(std::int64_t s) { seq = s; }
};
// Shapes copied from the live feed (values shortened).
const std::string kSnap =
    R"({"channel":"l2_data","timestamp":"2026-11-14T04:22:03.843671378Z","sequence_num":0,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"2026-11-14T04:22:03.552549Z","price_level":"66820.57","new_quantity":"0.03606"},{"side":"bid","event_time":"2026-11-14T04:22:03.552549Z","price_level":"66785","new_quantity":"0.3687208"},{"side":"offer","event_time":"2026-11-14T04:22:03.552549Z","price_level":"66820.6","new_quantity":"1.00000001"}]}]})";
const std::string kTrades =
    R"({"channel":"market_trades","timestamp":"2026-11-14T04:22:03.863732312Z","sequence_num":3,"events":[{"type":"update","trades":[{"product_id":"BTC-USD","trade_id":"1122523675","price":"66802.39","size":"0.00117472","time":"2026-11-14T04:22:03.729811Z","side":"BUY"},{"product_id":"BTC-USD","trade_id":"1122523674","price":"66807.67","size":"0.04176165","time":"2026-11-14T04:22:03.729811Z","side":"SELL"}]}]})";
}  // namespace

TEST(Coinbase, ParsesSnapshot) {
  Rec r;
  Stats st;
  ASSERT_TRUE(parse(kSnap.data(), kSnap.size(), r, st));
  EXPECT_EQ(r.snaps, 1);
  EXPECT_EQ(r.ends, 1);
  EXPECT_EQ(r.products[0], "BTC-USD");
  ASSERT_EQ(r.ups.size(), 3u);
  EXPECT_TRUE(r.ups[0].bid);
  EXPECT_EQ(r.ups[0].price, 6682057);
  EXPECT_EQ(r.ups[0].qty, 3606000);
  EXPECT_EQ(r.ups[1].price, 6678500);  // integer price, no decimals
  EXPECT_FALSE(r.ups[2].bid);
  EXPECT_EQ(r.ups[2].price, 6682060);  // one decimal padded
  EXPECT_EQ(r.ups[2].qty, 100000001);
  EXPECT_EQ(r.seq, 0);
  EXPECT_EQ(st.malformed, 0u);
}

TEST(Coinbase, ParsesTrades) {
  Rec r;
  Stats st;
  ASSERT_TRUE(parse(kTrades.data(), kTrades.size(), r, st));
  ASSERT_EQ(r.trades.size(), 2u);
  EXPECT_TRUE(r.trades[0].buy);
  EXPECT_EQ(r.trades[0].price, 6680239);
  EXPECT_EQ(r.trades[0].qty, 117472);
  EXPECT_FALSE(r.trades[1].buy);
  EXPECT_EQ(r.seq, 3);
}

TEST(Coinbase, FixedPointEdgeCases) {
  std::int64_t v;
  const char* nx;
  auto pf = [&](const char* s, int d) {
    const std::string t = std::string(s) + "\"";
    return parse_fixed(t.data(), t.data() + t.size(), d, v, nx) ? v : -1;
  };
  EXPECT_EQ(pf("0", 2), 0);
  EXPECT_EQ(pf("1.239", 2), 123);  // truncates
  EXPECT_EQ(pf("5.", 2), 500);
  EXPECT_EQ(pf(".5", 2), 50);
  EXPECT_EQ(pf("1.2.3", 2), -1);
  EXPECT_EQ(pf("-1", 2), -1);
  EXPECT_EQ(pf("", 2), -1);
  EXPECT_EQ(pf("99999999999999999999", 2), -1);  // overflow
}

TEST(Coinbase, EveryTruncationIsSafe) {
  // Every prefix of a valid message must parse cleanly or be rejected, never crash
  // (ASan build catches out-of-bounds reads).
  for (const std::string* m : {&kSnap, &kTrades}) {
    for (std::size_t n = 0; n < m->size(); ++n) {
      std::vector<char> buf(m->begin(), m->begin() + static_cast<long>(n));  // exact-size allocation
      Rec r;
      Stats st;
      parse(buf.data(), buf.size(), r, st);
    }
  }
}

TEST(Coinbase, UnknownChannelIsMalformed) {
  const std::string m = R"({"channel":"ticker","events":[]})";
  Rec r;
  Stats st;
  EXPECT_FALSE(parse(m.data(), m.size(), r, st));
  EXPECT_EQ(st.malformed, 1u);
}
