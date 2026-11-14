#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "lle/histogram.hpp"

using lle::Histogram;

TEST(Histogram, ExactBelow128) {
  Histogram h;
  for (int i = 0; i < 100; ++i) h.record(static_cast<std::uint64_t>(i));
  EXPECT_EQ(h.count(), 100u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.max(), 99u);
  EXPECT_EQ(h.percentile(50), 49u);
  EXPECT_EQ(h.percentile(100), 99u);
}

TEST(Histogram, IndexIsMonotonicAndBucketsContainValue) {
  std::uint64_t prev = 0;
  for (std::uint64_t v = 0; v < (1ull << 30); v = v * 3 / 2 + 1) {
    const int i = Histogram::index(v);
    EXPECT_GE(static_cast<std::uint64_t>(i), prev);
    prev = static_cast<std::uint64_t>(i);
    EXPECT_GE(Histogram::bucket_high(i), v);
    if (i > 0) {
      EXPECT_LT(Histogram::bucket_high(i - 1), v);
    }
  }
}

TEST(Histogram, PercentilesWithinBucketError) {
  std::mt19937_64 rng(1);
  std::lognormal_distribution<double> d(6.0, 1.0);
  std::vector<std::uint64_t> v;
  Histogram h;
  for (int i = 0; i < 200000; ++i) {
    const auto x = static_cast<std::uint64_t>(d(rng));
    v.push_back(x);
    h.record(x);
  }
  std::sort(v.begin(), v.end());
  for (double p : {50.0, 90.0, 99.0, 99.9}) {
    const auto exact = v[static_cast<std::size_t>(p / 100.0 * v.size()) - 1];
    const auto got = h.percentile(p);
    EXPECT_GE(got, exact) << p;
    EXPECT_LE(static_cast<double>(got), exact * 1.02 + 1) << p;
  }
  EXPECT_EQ(h.percentile(100), v.back());
}

TEST(Histogram, HugeValuesClampToLastBucket) {
  Histogram h;
  h.record(~0ull);
  EXPECT_EQ(h.max(), ~0ull);
  EXPECT_EQ(h.percentile(50), ~0ull);
}
