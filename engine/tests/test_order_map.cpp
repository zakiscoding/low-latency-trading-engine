#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lle/order_map.hpp"

using namespace lle;

TEST(OrderIdMap, InsertFindErase) {
  OrderIdMap m(10);
  EXPECT_EQ(m.find(5), OrderIdMap::kEmpty);
  EXPECT_TRUE(m.insert(5, 50));
  EXPECT_TRUE(m.insert(6, 60));
  EXPECT_EQ(m.find(5), 50u);
  EXPECT_TRUE(m.erase(5));
  EXPECT_EQ(m.find(5), OrderIdMap::kEmpty);
  EXPECT_EQ(m.find(6), 60u);
  EXPECT_FALSE(m.erase(5));
}

TEST(OrderIdMap, RefusesAboveLoadFactor) {
  OrderIdMap m(4);  // 16 slots, max 14
  int ok = 0;
  for (int i = 0; i < 20; ++i) ok += m.insert(static_cast<std::uint64_t>(i + 1), 1);
  EXPECT_EQ(ok, 14);
}

TEST(OrderIdMap, RandomOpsMatchUnorderedMap) {
  OrderIdMap m(14);
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(3);
  for (int i = 0; i < 300000; ++i) {
    const std::uint64_t k = rng() % 12000;  // dense, collides a lot after hashing
    if (rng() % 2 && ref.size() < 12000) {
      const auto v = static_cast<std::uint32_t>(rng() % 1000000);
      ASSERT_TRUE(m.insert(k, v));
      ref[k] = v;
    } else {
      ASSERT_EQ(m.erase(k), ref.erase(k) == 1);
    }
    const std::uint64_t q = rng() % 12000;
    auto it = ref.find(q);
    ASSERT_EQ(m.find(q), it == ref.end() ? OrderIdMap::kEmpty : it->second);
  }
  EXPECT_EQ(m.size(), ref.size());
}

TEST(Pool, AllocReleaseExhaust) {
  Pool<int> p(3);
  const auto a = p.alloc(), b = p.alloc(), c = p.alloc();
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);
  EXPECT_EQ(p.alloc(), Pool<int>::kNone);
  p.release(b);
  EXPECT_EQ(p.alloc(), b);
  EXPECT_EQ(p.used(), 3u);
}
