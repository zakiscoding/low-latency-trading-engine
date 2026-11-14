// Deterministic replay: the same input must give the same book and the same orders,
// run after run, and must match the golden digests checked in below. If an engine
// change alters behaviour on purpose, update the golden values in the same commit
// and say why in the message.
#include <gtest/gtest.h>

#include <cstdio>

#include "lle/itch_writer.hpp"
#include "lle/pipeline.hpp"

using namespace lle;

#ifndef LLE_GOLDEN_BOOK
#define LLE_GOLDEN_BOOK 0x5cdcf435dc2cfe58ull
#define LLE_GOLDEN_ORDERS 0x1cb76f7f1d71cf61ull
#endif

namespace {
struct Result {
  std::uint64_t book_digest;
  std::uint64_t order_digest;
  std::size_t orders;
};

Result replay(const std::vector<unsigned char>& day, int symbols, int packet_msgs) {
  static TscClock c = TscClock::calibrate(20);
  MarketBook mb;
  for (int i = 1; i <= symbols; ++i) mb.add_instrument(static_cast<std::uint16_t>(i));
  CaptureSender tx;
  NullTelemetry tel;
  StrategyParams sp;  // defaults: 60% imbalance, 1-tick spread
  RiskLimits rl;
  rl.max_orders_per_sec = 1'000'000;  // the rate limiter reads the TSC; keep it out of the result
  rl.max_position = 1'000'000;
  ItchEngine<MarketBook, CaptureSender> eng(mb, tx, tel, sp, rl, c);
  if (packet_msgs > 0) {
    for (const auto& p : itch::to_mold_packets(day, 1400, packet_msgs)) {
      eng.begin_packet(rdtsc());
      eng.on_packet(p.data(), p.size());
    }
  } else {
    std::size_t off = 0;
    while (off + 2 <= day.size()) {
      const std::size_t len = load_be<std::uint16_t>(day.data() + off);
      eng.on_message(day.data() + off + 2, len);
      off += 2 + len;
    }
  }
  std::uint64_t h = 1469598103934665603ull;
  for (auto a : tx.sent) {
    std::memset(a.data() + ord::kTsc, 0, 8);
    for (unsigned char ch : a) h = (h ^ ch) * 1099511628211ull;
  }
  return {mb.digest(), h, tx.sent.size()};
}
}  // namespace

TEST(ReplayDeterminism, SameInputSameOutput) {
  itch::SynthConfig cfg;
  cfg.messages = 400000;
  const auto day = itch::synth_day(cfg);
  const Result a = replay(day, cfg.symbols, 0);
  const Result b = replay(day, cfg.symbols, 0);
  const Result m1 = replay(day, cfg.symbols, 1);    // one message per packet
  const Result m8 = replay(day, cfg.symbols, 8);    // batched packets
  EXPECT_EQ(a.book_digest, b.book_digest);
  EXPECT_EQ(a.order_digest, b.order_digest);
  EXPECT_EQ(a.book_digest, m1.book_digest);
  EXPECT_EQ(a.order_digest, m1.order_digest);
  EXPECT_EQ(a.book_digest, m8.book_digest);
  EXPECT_EQ(a.order_digest, m8.order_digest);
  EXPECT_GT(a.orders, 0u);
  std::printf("golden: book=%#llx orders=%#llx n=%zu\n", static_cast<unsigned long long>(a.book_digest),
              static_cast<unsigned long long>(a.order_digest), a.orders);
}

// Golden values for seed 42 / 400k messages / 4 symbols. The synthetic generator
// uses std::mt19937_64 and std::geometric_distribution; libstdc++ and libc++ may
// differ in the distribution, so the golden check runs on libstdc++ only.
TEST(ReplayDeterminism, GoldenDigest) {
#if defined(_GLIBCXX_RELEASE)
  itch::SynthConfig cfg;
  cfg.messages = 400000;
  const auto day = itch::synth_day(cfg);
  const Result a = replay(day, cfg.symbols, 0);
  constexpr std::uint64_t kBook = LLE_GOLDEN_BOOK;
  constexpr std::uint64_t kOrders = LLE_GOLDEN_ORDERS;
  EXPECT_EQ(a.book_digest, kBook);
  EXPECT_EQ(a.order_digest, kOrders);
#else
  GTEST_SKIP() << "golden digests recorded with libstdc++";
#endif
}
