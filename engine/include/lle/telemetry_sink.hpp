#pragma once
// Engine-side telemetry: writes records into the shared-memory ring. Called after
// the order has been sent, so none of this is inside the measured tick-to-trade.
#include <cstdint>

#include "lle/book.hpp"
#include "lle/pipeline.hpp"
#include "lle/shm.hpp"
#include "lle/tsc.hpp"

namespace lle {

class ShmTelemetry {
 public:
  ShmTelemetry(shm::TelemetryRing& ring, const TscClock& clk) : ring_(ring), clk_(clk) {}

  LLE_ALWAYS_INLINE void on_order(int b, Side s, std::int64_t px, std::int64_t qty, std::uint64_t token) noexcept {
    ring_.push(shm::kOrder, static_cast<std::uint16_t>(b), static_cast<std::uint8_t>(s), 0, px, qty,
               static_cast<std::int64_t>(token), 0, 0, now());
  }
  LLE_ALWAYS_INLINE void on_top(int b, const Top& t) noexcept {
    // Rate-limit top-of-book records: the UI needs ~20/s, the feed gives far more.
    const std::uint64_t ts = rdtsc_unordered();
    if (ts - last_top_[b & 63] < top_gap_cycles_) return;
    last_top_[b & 63] = ts;
    ring_.push(shm::kTop, static_cast<std::uint16_t>(b), 0, 0, t.bid, t.bid_qty, t.ask, t.ask_qty, 0, now());
  }
  LLE_ALWAYS_INLINE void on_trade(int b, Side resting, std::int64_t px, std::int64_t qty) noexcept {
    ring_.push(shm::kTrade, static_cast<std::uint16_t>(b), static_cast<std::uint8_t>(resting), 0, px, qty, 0, 0, 0,
               now());
  }
  // Stage durations in cycles; stored in ns.
  LLE_ALWAYS_INLINE void on_t2t(std::uint64_t parse, std::uint64_t book, std::uint64_t decide, std::uint64_t build,
                                std::uint64_t send, std::uint64_t total) noexcept {
    pending_ = Pending{parse, book, decide, build, send, total, true};
  }
  // Called by the main loop once the wire-level number is known (or 0).
  void flush_latency(std::int64_t wire_ns) noexcept {
    if (!pending_.valid) return;
    const auto& p = pending_;
    // Pack parse/book/decide in a, b, c; build/send in d (hi/lo 32 bits); total in e; wire in u32 (us capped).
    const std::int64_t d = (static_cast<std::int64_t>(clk_.to_ns(p.build)) << 32) |
                           static_cast<std::int64_t>(clk_.to_ns(p.send) & 0xFFFFFFFF);
    ring_.push(shm::kLatency, 0, 0, static_cast<std::uint32_t>(wire_ns > 0 ? (wire_ns > 0xFFFFFFFF ? 0xFFFFFFFF : wire_ns) : 0),
               static_cast<std::int64_t>(clk_.to_ns(p.parse)), static_cast<std::int64_t>(clk_.to_ns(p.book)),
               static_cast<std::int64_t>(clk_.to_ns(p.decide)), d, static_cast<std::int64_t>(clk_.to_ns(p.total)), now());
    pending_.valid = false;
  }
  void on_fill(int b, const ExecReport& r) noexcept {
    ring_.push(shm::kFill, static_cast<std::uint16_t>(b), static_cast<std::uint8_t>(r.side),
               static_cast<std::uint32_t>(r.liquidity), r.price, r.qty, static_cast<std::int64_t>(r.token),
               r.fee_micros, 0, now());
  }
  void stats(std::int64_t msgs, std::int64_t orders, std::int64_t fills, std::int64_t rejects, std::int64_t pos0) noexcept {
    ring_.push(shm::kStats, 0, 0, 0, msgs, orders, fills, rejects, pos0, now());
  }
  // Depth snapshot: top `depth` levels each side, bracketed by begin/end markers.
  void snapshot(const MarketBook& mb, int depth) noexcept {
    const std::int64_t ts = now();
    for (std::size_t b = 0; b < mb.num_books(); ++b) {
      const auto& ib = mb.book(static_cast<int>(b));
      ring_.push(shm::kSnapBegin, static_cast<std::uint16_t>(b), 0, 0, 0, 0, 0, 0, 0, ts);
      std::uint32_t k = 0;
      ib.bids.for_each_level(depth, [&](std::int64_t p, std::int64_t q, std::uint32_t n) {
        ring_.push(shm::kLevel, static_cast<std::uint16_t>(b), 0, k++, p, q, n, 0, 0, ts);
      });
      k = 0;
      ib.asks.for_each_level(depth, [&](std::int64_t p, std::int64_t q, std::uint32_t n) {
        ring_.push(shm::kLevel, static_cast<std::uint16_t>(b), 1, k++, p, q, n, 0, 0, ts);
      });
      ring_.push(shm::kSnapEnd, static_cast<std::uint16_t>(b), 0, 0, 0, 0, 0, 0, 0, ts);
    }
  }
  void set_top_rate_hz(double hz) noexcept { top_gap_cycles_ = static_cast<std::uint64_t>(clk_.ticks_per_ns * 1e9 / hz); }

 private:
  std::int64_t now() const noexcept { return clk_.to_realtime_ns(rdtsc_unordered()); }
  struct Pending {
    std::uint64_t parse, book, decide, build, send, total;
    bool valid = false;
  };
  shm::TelemetryRing& ring_;
  const TscClock& clk_;
  Pending pending_{};
  std::uint64_t last_top_[64]{};
  std::uint64_t top_gap_cycles_ = 0;
};

}  // namespace lle
