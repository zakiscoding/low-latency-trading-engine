#include <gtest/gtest.h>

#include <vector>

#include "lle/paper_exchange.hpp"

using namespace lle;

namespace {
struct Sink {
  std::vector<ExecReport> r;
  static void fn(void* c, const ExecReport& e, int) { static_cast<Sink*>(c)->r.push_back(e); }
  std::vector<ExecReport> fills() const {
    std::vector<ExecReport> f;
    for (auto& e : r) if (e.type == 'F') f.push_back(e);
    return f;
  }
};

OrderTemplate order(Side s, char tif, std::uint64_t token, std::uint32_t qty, std::int64_t px) {
  OrderTemplate t;
  t.init('O', s, tif, 0, 1);
  t.patch(token, qty, px, 0, 0);
  return t;
}

PaperConfig cfg() {
  PaperConfig c;
  c.latency_ns = 1000;
  c.levels_log2 = 12;
  return c;
}
}  // namespace

TEST(PaperExchange, LatencyDelaysOrders) {
  Sink s;
  PaperExchange x(cfg(), 1, &Sink::fn, &s);
  x.on_market_level(0, Side::Sell, 101, 50);
  auto o = order(Side::Buy, 'I', 1, 10, 101);
  x.submit(OrderMsgView{o.bytes}, 0);
  x.tick(999);
  EXPECT_TRUE(s.r.empty());
  x.tick(1000);
  ASSERT_EQ(s.fills().size(), 1u);
  EXPECT_EQ(s.fills()[0].price, 101);
  EXPECT_EQ(s.fills()[0].liquidity, 'T');
  EXPECT_EQ(s.fills()[0].fee_micros, 3000 * 10);  // $0.003/share taker
}

TEST(PaperExchange, TakerWalksLevelsAndDoesNotReuseLiquidity) {
  Sink s;
  PaperExchange x(cfg(), 1, &Sink::fn, &s);
  x.on_market_level(0, Side::Sell, 101, 5);
  x.on_market_level(0, Side::Sell, 102, 5);
  auto o1 = order(Side::Buy, 'I', 1, 8, 102);
  x.submit(OrderMsgView{o1.bytes}, 0);
  x.tick(5000);
  auto f = s.fills();
  ASSERT_EQ(f.size(), 2u);
  EXPECT_EQ(f[0].qty, 5u);
  EXPECT_EQ(f[1].qty, 3u);
  EXPECT_EQ(f[1].price, 102);
  // Same displayed book, second order: only 2 left at 102.
  auto o2 = order(Side::Buy, 'I', 2, 8, 102);
  x.submit(OrderMsgView{o2.bytes}, 5000);
  x.tick(10000);
  f = s.fills();
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(f[2].qty, 2u);
  EXPECT_EQ(x.account().position[0], 10);
}

TEST(PaperExchange, RestingOrderWaitsForQueueAhead) {
  Sink s;
  PaperExchange x(cfg(), 1, &Sink::fn, &s);
  x.on_market_level(0, Side::Buy, 100, 300);  // 300 ahead of us
  x.on_market_level(0, Side::Sell, 101, 100);
  auto o = order(Side::Buy, 'D', 7, 50, 100);
  x.submit(OrderMsgView{o.bytes}, 0);
  x.tick(2000);
  EXPECT_TRUE(s.fills().empty());
  EXPECT_EQ(x.resting(0), 1u);
  x.on_market_trade(0, Side::Buy, 100, 200);  // burns 200 of the 300 ahead
  EXPECT_TRUE(s.fills().empty());
  x.on_market_level(0, Side::Buy, 100, 60);   // cancels ahead: queue_ahead -> 60
  x.on_market_trade(0, Side::Buy, 100, 80);   // 60 ahead, 20 fill us
  auto f = s.fills();
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0].qty, 20u);
  EXPECT_EQ(f[0].liquidity, 'M');
  EXPECT_EQ(f[0].fee_micros, -2000 * 20);  // rebate
  x.on_market_trade(0, Side::Buy, 99, 1);  // trades through us: fill the rest
  f = s.fills();
  ASSERT_EQ(f.size(), 2u);
  EXPECT_EQ(f[1].qty, 30u);
  EXPECT_EQ(f[1].price, 100);
  EXPECT_EQ(x.resting(0), 0u);
}

TEST(PaperExchange, CrossingTouchFillsRestingOrder) {
  Sink s;
  PaperExchange x(cfg(), 1, &Sink::fn, &s);
  x.on_market_level(0, Side::Sell, 105, 10);
  auto o = order(Side::Buy, 'D', 9, 5, 103);
  x.submit(OrderMsgView{o.bytes}, 0);
  x.tick(2000);
  EXPECT_TRUE(s.fills().empty());
  x.on_market_level(0, Side::Sell, 103, 7);  // someone offers at our bid
  ASSERT_EQ(s.fills().size(), 1u);
  EXPECT_EQ(s.fills()[0].qty, 5u);
}

TEST(PaperExchange, BpsFeesForCrypto) {
  Sink s;
  PaperConfig c = cfg();
  c.qty_scale = 100'000'000;  // 1e8 units per BTC
  c.fees.kind = FeeModel::Bps;
  c.fees.taker = 12000;  // 1.20%
  c.fees.maker = 6000;
  PaperExchange x(c, 1, &Sink::fn, &s);
  x.on_market_level(0, Side::Sell, 10'000'000, 100'000'000);  // $100,000.00 x 1 BTC
  auto o = order(Side::Buy, 'I', 1, 1'000'000, 10'000'000);    // 0.01 BTC
  x.submit(OrderMsgView{o.bytes}, 0);
  x.tick(2000);
  ASSERT_EQ(s.fills().size(), 1u);
  // notional $1000 -> 1.2% = $12 = 12,000,000 micros
  EXPECT_EQ(s.fills()[0].fee_micros, 12'000'000);
  EXPECT_EQ(x.account().cash_micros, -1'000'000'000 - 12'000'000);
}
