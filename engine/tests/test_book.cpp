#include <gtest/gtest.h>

#include "lle/book.hpp"
#include "lle/itch.hpp"
#include "lle/itch_writer.hpp"
#include "lle/naive_book.hpp"

using namespace lle;

TEST(MarketBook, AddExecuteCancelDelete) {
  MarketBook mb({.levels_log2 = 12, .order_map_log2 = 10, .order_pool = 512});
  mb.add_instrument(5);
  auto e = mb.add(1, 5, Side::Buy, 10000, 100);  // $1.00 -> 100 ticks
  EXPECT_EQ(e.book, 0);
  EXPECT_TRUE(e.top_changed);
  e = mb.add(2, 5, Side::Buy, 9900, 50);
  EXPECT_FALSE(e.top_changed);
  e = mb.add(3, 5, Side::Sell, 10100, 70);
  EXPECT_TRUE(e.top_changed);
  const auto& b = mb.book(0);
  EXPECT_EQ(b.bids.best_price(), 100);
  EXPECT_EQ(b.asks.best_price(), 101);

  std::int64_t px = 0;
  Side s{};
  e = mb.reduce(1, 40, &px, &s);
  EXPECT_EQ(px, 100);
  EXPECT_EQ(s, Side::Buy);
  EXPECT_TRUE(e.top_changed);
  EXPECT_EQ(b.bids.best_qty(), 60);
  e = mb.reduce(1, 60);  // fully executed -> removed
  EXPECT_EQ(b.bids.best_price(), 99);
  EXPECT_EQ(mb.live_orders(), 2u);
  e = mb.remove(1);  // already gone
  EXPECT_EQ(e.book, -1);
  e = mb.replace(3, 4, 10, 10200);
  EXPECT_EQ(b.asks.best_price(), 102);
  EXPECT_EQ(b.asks.best_qty(), 10);
  mb.remove(4);
  EXPECT_TRUE(b.asks.empty());
}

TEST(MarketBook, UntrackedLocateIgnored) {
  MarketBook mb({.levels_log2 = 12, .order_map_log2 = 10, .order_pool = 64});
  mb.add_instrument(1);
  EXPECT_EQ(mb.add(1, 2, Side::Buy, 100, 1).book, -1);
  EXPECT_EQ(mb.live_orders(), 0u);
}

TEST(MarketBook, PoolExhaustionIsCountedNotFatal) {
  MarketBook mb({.levels_log2 = 12, .order_map_log2 = 6, .order_pool = 4});
  mb.add_instrument(1);
  for (int i = 0; i < 10; ++i) mb.add(static_cast<std::uint64_t>(i + 1), 1, Side::Buy, 10000, 1);
  EXPECT_EQ(mb.live_orders(), 4u);
  EXPECT_EQ(mb.pool_exhausted(), 6u);
}

namespace {
template <class B>
struct Apply : itch::NullHandler {
  B& b;
  explicit Apply(B& bb) : b(bb) {}
  void on_add(const itch::AddOrder& m) { b.add(m.ref(), m.locate(), m.side(), m.price(), m.shares()); }
  void on_executed(const itch::OrderExecuted& m) { b.reduce(m.ref(), m.shares()); }
  void on_cancel(const itch::OrderCancel& m) { b.reduce(m.ref(), m.shares()); }
  void on_delete(const itch::OrderDelete& m) { b.remove(m.ref()); }
  void on_replace(const itch::OrderReplace& m) { b.replace(m.old_ref(), m.new_ref(), m.shares(), m.price()); }
};
}  // namespace

// The flat book and the std::map book must agree on every level after a synthetic day.
TEST(MarketBook, MatchesNaiveBookOnSyntheticDay) {
  itch::SynthConfig cfg;
  cfg.messages = 300000;
  cfg.symbols = 3;
  const auto day = itch::synth_day(cfg);
  MarketBook flat({.levels_log2 = 15, .order_map_log2 = 18, .order_pool = 1 << 17});
  NaiveBook naive;
  for (int i = 1; i <= cfg.symbols; ++i) {
    flat.add_instrument(static_cast<std::uint16_t>(i));
    naive.add_instrument(static_cast<std::uint16_t>(i));
  }
  Apply<MarketBook> af(flat);
  Apply<NaiveBook> an(naive);
  itch::ParseStats s1, s2;
  itch::parse_stream(day.data(), day.size(), af, s1);
  itch::parse_stream(day.data(), day.size(), an, s2);
  EXPECT_EQ(flat.digest(), naive.digest());
  for (int b = 0; b < cfg.symbols; ++b) {
    EXPECT_EQ(flat.book(b).bids.best_price(), naive.best_bid(b));
    EXPECT_EQ(flat.book(b).asks.best_price(), naive.best_ask(b));
    EXPECT_EQ(flat.book(b).dropped_out_of_window, 0u);
  }
}
