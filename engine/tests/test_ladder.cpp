#include <gtest/gtest.h>

#include <map>
#include <random>

#include "lle/ladder.hpp"

using namespace lle;

TEST(Ladder, BidBestTracksHighest) {
  BidLadder b(12);
  EXPECT_TRUE(b.empty());
  b.add(1000, 5);
  b.add(1002, 3);
  b.add(999, 7);
  EXPECT_EQ(b.best_price(), 1002);
  EXPECT_EQ(b.best_qty(), 3);
  b.reduce(1002, 3, 1);
  EXPECT_EQ(b.best_price(), 1000);
  b.reduce(1000, 5, 1);
  EXPECT_EQ(b.best_price(), 999);
  b.reduce(999, 7, 1);
  EXPECT_TRUE(b.empty());
}

TEST(Ladder, AskBestTracksLowest) {
  AskLadder a(12);
  a.add(1005, 1);
  a.add(1003, 2);
  a.add(1010, 3);
  EXPECT_EQ(a.best_price(), 1003);
  a.reduce(1003, 2);
  EXPECT_EQ(a.best_price(), 1005);
  EXPECT_EQ(a.next_worse(1005), 1010);
  EXPECT_EQ(a.next_worse(1010), AskLadder::kNone);
}

TEST(Ladder, OutOfWindowIsDropped) {
  BidLadder b(12);  // 4096 levels, centered on first price
  EXPECT_TRUE(b.add(10000, 1));
  EXPECT_FALSE(b.add(10000 + 5000, 1));
  EXPECT_FALSE(b.add(10000 - 5000, 1));
  EXPECT_EQ(b.best_price(), 10000);
}

TEST(Ladder, SetRecentersNearTouch) {
  AskLadder a(12);
  a.set(10000, 4);
  a.set(10000 + 1500, 2);  // inside window
  a.set(10000 + 2100, 1);  // outside window, but within n/4 of... no: 2100 > 1024 -> ignored
  EXPECT_EQ(a.qty_at(10000 + 2100), 0);
  a.set(10000, 0);
  // touch is now 11500; a price 2100+ above base is within n/4 of touch -> recenter
  a.set(10000 + 2100, 1);
  EXPECT_EQ(a.recenters(), 1u);
  EXPECT_EQ(a.best_price(), 11500);
  EXPECT_EQ(a.qty_at(12100), 1);
}

// Randomised comparison against std::map across word and summary boundaries.
template <bool IsBid>
void fuzz_against_map(std::uint64_t seed) {
  PriceLadder<IsBid> l(16);
  std::map<std::int64_t, std::int64_t> ref;
  std::mt19937_64 rng(seed);
  const std::int64_t center = 1'000'000;
  l.add(center, 1);
  ref[center] = 1;
  for (int i = 0; i < 200000; ++i) {
    const std::int64_t px = center - 30000 + static_cast<std::int64_t>(rng() % 60000);
    if (rng() % 3 == 0 && ref.count(px)) {
      l.reduce(px, ref[px], 1);
      ref.erase(px);
    } else {
      const std::int64_t q = 1 + static_cast<std::int64_t>(rng() % 100);
      l.add(px, q);
      ref[px] += q;
    }
    if (ref.empty()) {
      ASSERT_TRUE(l.empty());
      continue;
    }
    const std::int64_t want = IsBid ? ref.rbegin()->first : ref.begin()->first;
    ASSERT_EQ(l.best_price(), want) << "iter " << i;
  }
  // Walk the whole side.
  std::vector<std::int64_t> got;
  l.for_each_level(1 << 20, [&](std::int64_t p, std::int64_t q, std::uint32_t) {
    got.push_back(p);
    EXPECT_EQ(q, ref[p]);
  });
  ASSERT_EQ(got.size(), ref.size());
}

TEST(Ladder, RandomBidMatchesMap) { fuzz_against_map<true>(7); }
TEST(Ladder, RandomAskMatchesMap) { fuzz_against_map<false>(8); }

// clear() walks the bitmap instead of zeroing the whole window: everything must
// really be gone (levels, bits, best) and the ladder reusable at a new price.
TEST(Ladder, ClearIsCompleteAndReusable) {
  AskLadder a(16);
  for (int round = 0; round < 3; ++round) {
    const std::int64_t base = 1'000'000 + round * 50'000;
    for (int i = 0; i < 5000; i += 7) a.set(base + i, 1 + i);
    EXPECT_EQ(a.best_price(), base);
    a.clear();
    EXPECT_TRUE(a.empty());
    for (int i = 0; i < 5000; i += 7) EXPECT_EQ(a.qty_at(base + i), 0);
    int n = 0;
    a.for_each_level(1 << 20, [&](std::int64_t, std::int64_t, std::uint32_t) { n++; });
    EXPECT_EQ(n, 0);
  }
}
