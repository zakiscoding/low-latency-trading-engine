#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "lle/matching.hpp"

using namespace lle;

namespace {
struct Rec : NullMatchHandler {
  struct Fill { std::uint64_t maker, taker; std::int64_t px; std::uint32_t q; };
  std::vector<Fill> fills;
  std::vector<std::uint64_t> rested, done;
  std::vector<char> rejects;
  void on_fill(const MeOrder& m, std::uint64_t t, std::uint64_t, std::int64_t px, std::uint32_t q) {
    fills.push_back({m.id, t, px, q});
  }
  void on_rest(const MeOrder& m) { rested.push_back(m.id); }
  void on_done(std::uint64_t id, std::uint32_t) { done.push_back(id); }
  void on_reject(std::uint64_t, char r) { rejects.push_back(r); }
};
}  // namespace

TEST(Matching, PriceTimePriority) {
  MatchingEngine me(12, 1024, 12);
  Rec r;
  me.submit(1, 0, Side::Sell, 101, 10, Tif::Day, 0, r);
  me.submit(2, 0, Side::Sell, 100, 5, Tif::Day, 0, r);
  me.submit(3, 0, Side::Sell, 100, 5, Tif::Day, 0, r);  // behind order 2 at 100
  EXPECT_EQ(me.asks().best_price(), 100);
  me.submit(4, 0, Side::Buy, 101, 12, Tif::Ioc, 0, r);
  ASSERT_EQ(r.fills.size(), 3u);
  EXPECT_EQ(r.fills[0].maker, 2u);
  EXPECT_EQ(r.fills[1].maker, 3u);
  EXPECT_EQ(r.fills[2].maker, 1u);
  EXPECT_EQ(r.fills[2].q, 2u);
  EXPECT_EQ(r.fills[2].px, 101);  // maker's price
  EXPECT_EQ(me.asks().best_qty(), 8);
  EXPECT_EQ(me.resting(), 1u);
}

TEST(Matching, CancelAndIocRemainder) {
  MatchingEngine me(12, 1024, 12);
  Rec r;
  me.submit(1, 0, Side::Buy, 100, 10, Tif::Day, 0, r);
  EXPECT_TRUE(me.cancel(1, r));
  EXPECT_FALSE(me.cancel(1, r));
  EXPECT_TRUE(me.bids().empty());
  me.submit(2, 0, Side::Sell, 99, 10, Tif::Ioc, 0, r);  // nothing to hit
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(me.resting(), 0u);
}

TEST(Matching, Rejects) {
  MatchingEngine me(12, 2, 4);
  Rec r;
  me.submit(1, 0, Side::Buy, 100, 0, Tif::Day, 0, r);
  me.submit(2, 0, Side::Buy, 100, 1, Tif::Day, 0, r);
  me.submit(2, 0, Side::Buy, 100, 1, Tif::Day, 0, r);
  me.submit(3, 0, Side::Buy, 100 + 10000, 1, Tif::Day, 0, r);
  me.submit(4, 0, Side::Buy, 99, 1, Tif::Day, 0, r);
  me.submit(5, 0, Side::Buy, 98, 1, Tif::Day, 0, r);  // pool of 2 is full
  EXPECT_EQ(r.rejects, (std::vector<char>{'Q', 'D', 'P', 'F'}));
}

// Conservation: random flow, every unit is either resting, filled (twice: maker
// and taker side) or expired.
TEST(Matching, RandomFlowConservesQuantity) {
  MatchingEngine me(14, 1 << 15, 17);
  struct Count : NullMatchHandler {
    std::uint64_t filled = 0;
    void on_fill(const MeOrder&, std::uint64_t, std::uint64_t, std::int64_t, std::uint32_t q) { filled += q; }
  } h;
  std::mt19937_64 rng(5);
  std::uint64_t submitted_resting_or_filled = 0;
  std::int64_t expired = 0;
  struct Exp : Count {
    std::int64_t* e;
    void on_done(std::uint64_t, std::uint32_t left) { *e += left; }
  } eh;
  eh.e = &expired;
  std::vector<std::uint64_t> live;
  for (std::uint64_t id = 1; id < 100000; ++id) {
    const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
    const std::int64_t px = 1000 + static_cast<std::int64_t>(rng() % 21) - 10;
    const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng() % 50);
    if (rng() % 10 == 0 && !live.empty()) {
      const std::size_t k = rng() % live.size();
      me.cancel(live[k], eh);
      live[k] = live.back();
      live.pop_back();
      continue;
    }
    me.submit(id, 0, s, px, q, (rng() % 4 == 0) ? Tif::Ioc : Tif::Day, 0, eh);
    submitted_resting_or_filled += q;
    live.push_back(id);
  }
  std::int64_t resting = 0;
  me.bids().for_each_level(1 << 20, [&](std::int64_t, std::int64_t q, std::uint32_t) { resting += q; });
  me.asks().for_each_level(1 << 20, [&](std::int64_t, std::int64_t q, std::uint32_t) { resting += q; });
  // submitted = resting + 2*filled (both sides of each fill were submitted) + expired/cancelled
  EXPECT_EQ(static_cast<std::int64_t>(submitted_resting_or_filled), resting + 2 * static_cast<std::int64_t>(eh.filled) + expired);
  // Book never crossed.
  if (!me.bids().empty() && !me.asks().empty()) {
    EXPECT_LT(me.bids().best_price(), me.asks().best_price());
  }
}
