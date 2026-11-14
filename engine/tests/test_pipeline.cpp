#include <gtest/gtest.h>

#include "lle/itch_writer.hpp"
#include "lle/pipeline.hpp"

using namespace lle;

namespace {
TscClock clk() {
  static TscClock c = TscClock::calibrate(20);
  return c;
}
StrategyParams eager() {
  StrategyParams p;
  p.imbalance_num = 1;
  p.imbalance_den = 2;  // act at 50% imbalance
  p.order_qty = 10;
  p.position_limit = 1000;
  return p;
}
RiskLimits loose() {
  RiskLimits r;
  r.max_orders_per_sec = 1'000'000;
  r.max_position = 100000;
  r.max_notional_ticks = 1ll << 40;
  return r;
}
}  // namespace

TEST(Pipeline, ImbalanceSendsBuyAtAsk) {
  MarketBook mb({.levels_log2 = 12, .order_map_log2 = 10, .order_pool = 256});
  mb.add_instrument(1);
  CaptureSender tx;
  NullTelemetry tel;
  auto c = clk();
  ItchEngine<MarketBook, CaptureSender> eng(mb, tx, tel, eager(), loose(), c);
  itch::Writer w;
  w.add(1, 1, 1, Side::Sell, 100, "T", 1'000'100);  // ask 10001 x100
  w.add(1, 2, 2, Side::Buy, 900, "T", 1'000'000);   // bid 10000 x900 -> imbalance .8
  std::size_t off = 0;
  while (off < w.buf.size()) {
    const std::size_t len = load_be<std::uint16_t>(w.buf.data() + off);
    eng.on_message(w.buf.data() + off + 2, len);
    off += 2 + len;
  }
  ASSERT_EQ(tx.sent.size(), 1u);
  OrderMsgView o{tx.sent[0].data()};
  EXPECT_EQ(o.type(), 'O');
  EXPECT_EQ(o.side(), Side::Buy);
  EXPECT_EQ(o.price(), 10001);
  EXPECT_EQ(o.qty(), 10u);
  EXPECT_EQ(o.token(), 1u);
  EXPECT_EQ(o.feed_ts(), 2);
  EXPECT_EQ(eng.hists().h[kTickToTrade].count(), 1u);
}

TEST(Pipeline, KillSwitchBlocksOrders) {
  MarketBook mb({.levels_log2 = 12, .order_map_log2 = 10, .order_pool = 256});
  mb.add_instrument(1);
  CaptureSender tx;
  NullTelemetry tel;
  auto c = clk();
  ItchEngine<MarketBook, CaptureSender> eng(mb, tx, tel, eager(), loose(), c);
  eng.risk().kill();
  itch::Writer w;
  w.add(1, 1, 1, Side::Sell, 100, "T", 1'000'100);
  w.add(1, 2, 2, Side::Buy, 900, "T", 1'000'000);
  itch::ParseStats st;
  std::size_t off = 0;
  while (off < w.buf.size()) {
    const std::size_t len = load_be<std::uint16_t>(w.buf.data() + off);
    eng.on_message(w.buf.data() + off + 2, len);
    off += 2 + len;
  }
  EXPECT_TRUE(tx.sent.empty());
  EXPECT_EQ(eng.counters().risk_rejects, 1u);
  EXPECT_EQ(eng.counters().reject_by_reason[static_cast<int>(RiskReject::KillSwitch)], 1u);
}

TEST(Pipeline, RiskRejectsOversizeAndPosition) {
  auto c = clk();
  RiskLimits r;
  r.max_order_qty = 5;
  r.max_position = 8;
  RiskGate g(r, c.ticks_per_ns);
  EXPECT_EQ(g.check(Side::Buy, 6, 100, 0, 100, rdtsc()), RiskReject::Qty);
  EXPECT_EQ(g.check(Side::Buy, 5, 100, 5, 100, rdtsc()), RiskReject::Position);
  EXPECT_EQ(g.check(Side::Sell, 5, 100, -5, 100, rdtsc()), RiskReject::Position);
  EXPECT_EQ(g.check(Side::Buy, 5, 100, 0, 1000, rdtsc()), RiskReject::PriceBand);
  EXPECT_EQ(g.check(Side::Buy, 5, 100, 0, 100, rdtsc()), RiskReject::None);
  g.on_pnl(-r.max_daily_loss_ticks - 1);
  EXPECT_EQ(g.check(Side::Buy, 1, 100, 0, 100, rdtsc()), RiskReject::DailyLoss);
  EXPECT_TRUE(g.killed());
}

TEST(Pipeline, RateLimiterCapsBurst) {
  auto c = clk();
  RiskLimits r;
  r.max_orders_per_sec = 10;
  RiskGate g(r, c.ticks_per_ns);
  int ok = 0;
  const std::uint64_t now = rdtsc();
  for (int i = 0; i < 100; ++i) ok += g.check(Side::Buy, 1, 100, 0, 100, now) == RiskReject::None;
  EXPECT_EQ(ok, 10);
}

TEST(Pipeline, AllVariantsSendTheSameOrders) {
  itch::SynthConfig cfg;
  cfg.messages = 100000;
  cfg.symbols = 2;
  const auto day = itch::synth_day(cfg);
  auto c = clk();
  auto run = [&](auto& eng) {
    std::size_t off = 0;
    while (off + 2 <= day.size()) {
      const std::size_t len = load_be<std::uint16_t>(day.data() + off);
      eng.on_message(day.data() + off + 2, len);
      off += 2 + len;
    }
  };
  std::vector<std::array<unsigned char, kOrderMsgSize>> ref;
  auto strip = [](std::vector<std::array<unsigned char, kOrderMsgSize>> v) {
    for (auto& a : v) std::memset(a.data() + ord::kTsc, 0, 8);  // TSC differs per run
    return v;
  };
  {
    MarketBook mb;
    mb.add_instrument(1);
    mb.add_instrument(2);
    CaptureSender tx;
    NullTelemetry tel;
    ItchEngine<MarketBook, CaptureSender> eng(mb, tx, tel, eager(), loose(), c);
    run(eng);
    ref = strip(tx.sent);
    EXPECT_GT(ref.size(), 100u);
  }
  {
    NaiveBook nb;
    nb.add_instrument(1);
    nb.add_instrument(2);
    CaptureSender tx;
    NullTelemetry tel;
    ItchEngine<NaiveBook, CaptureSender, TextBuilder, NullTelemetry, true, true> eng(nb, tx, tel, eager(), loose(), c);
    run(eng);
    const auto got = strip(tx.sent);
    ASSERT_EQ(got.size(), ref.size());
    EXPECT_TRUE(got == ref);
  }
}
