#pragma once
// Pre-trade risk checks, enforced inside the engine even in paper mode.
// All checks are integer compares on one cache line; the order-rate limiter is a
// token bucket refilled from the TSC, so there's no clock syscall.
#include <atomic>
#include <cstdint>

#include "lle/common.hpp"

namespace lle {

struct RiskLimits {
  std::int64_t max_order_qty = 100;          // per order
  std::int64_t max_position = 500;           // absolute, per instrument
  std::int64_t max_notional_ticks = 5'000'000;  // qty * price (ticks) per order
  std::uint32_t max_orders_per_sec = 50;
  std::uint32_t max_orders_per_day = 20'000;
  std::int64_t max_daily_loss_ticks = 1'000'000;  // realised + fees, in ticks*qty
  std::int64_t price_band_ticks = 500;       // reject orders further than this from the touch
};

enum class RiskReject : std::uint8_t {
  None = 0,
  KillSwitch,
  Qty,
  Position,
  Notional,
  Rate,
  DailyCount,
  DailyLoss,
  PriceBand,
};

class alignas(kCacheLine) RiskGate {
 public:
  explicit RiskGate(const RiskLimits& lim, double ticks_per_ns) : lim_(lim) {
    cycles_per_token_ = static_cast<std::uint64_t>(ticks_per_ns * 1e9 / (lim.max_orders_per_sec ? lim.max_orders_per_sec : 1));
    tokens_ = lim.max_orders_per_sec;
  }

  void set_limits(const RiskLimits& lim, double ticks_per_ns) noexcept {
    lim_ = lim;
    cycles_per_token_ = static_cast<std::uint64_t>(ticks_per_ns * 1e9 / (lim.max_orders_per_sec ? lim.max_orders_per_sec : 1));
  }

  // position: current signed position for the instrument (shares/contracts).
  LLE_ALWAYS_INLINE RiskReject check(Side side, std::int64_t qty, std::int64_t price, std::int64_t position,
                                     std::int64_t touch, std::uint64_t now_tsc) noexcept {
    if (LLE_UNLIKELY(killed_.load(std::memory_order_relaxed))) return RiskReject::KillSwitch;
    if (LLE_UNLIKELY(qty <= 0 || qty > lim_.max_order_qty)) return RiskReject::Qty;
    const std::int64_t after = side == Side::Buy ? position + qty : position - qty;
    if (LLE_UNLIKELY(after > lim_.max_position || -after > lim_.max_position)) return RiskReject::Position;
    if (LLE_UNLIKELY(qty * price > lim_.max_notional_ticks)) return RiskReject::Notional;
    const std::int64_t d = price - touch;
    if (LLE_UNLIKELY(d > lim_.price_band_ticks || -d > lim_.price_band_ticks)) return RiskReject::PriceBand;
    if (LLE_UNLIKELY(orders_today_ >= lim_.max_orders_per_day)) return RiskReject::DailyCount;
    if (LLE_UNLIKELY(-pnl_ticks_ > lim_.max_daily_loss_ticks)) {
      killed_.store(true, std::memory_order_relaxed);  // loss limit trips the kill switch
      return RiskReject::DailyLoss;
    }
    // Token bucket.
    if (now_tsc - last_refill_ >= cycles_per_token_) {
      const std::uint64_t add = (now_tsc - last_refill_) / cycles_per_token_;
      tokens_ = static_cast<std::uint32_t>(tokens_ + add > lim_.max_orders_per_sec ? lim_.max_orders_per_sec : tokens_ + add);
      last_refill_ += add * cycles_per_token_;
    }
    if (LLE_UNLIKELY(tokens_ == 0)) return RiskReject::Rate;
    tokens_--;
    orders_today_++;
    return RiskReject::None;
  }

  void on_pnl(std::int64_t delta_ticks) noexcept { pnl_ticks_ += delta_ticks; }
  void kill() noexcept { killed_.store(true, std::memory_order_relaxed); }
  void unkill() noexcept { killed_.store(false, std::memory_order_relaxed); }
  bool killed() const noexcept { return killed_.load(std::memory_order_relaxed); }
  std::uint32_t orders_today() const noexcept { return orders_today_; }
  const RiskLimits& limits() const noexcept { return lim_; }

 private:
  RiskLimits lim_;
  std::atomic<bool> killed_{false};
  std::uint32_t tokens_ = 0;
  std::uint32_t orders_today_ = 0;
  std::uint64_t last_refill_ = 0;
  std::uint64_t cycles_per_token_ = 1;
  std::int64_t pnl_ticks_ = 0;
};

}  // namespace lle
