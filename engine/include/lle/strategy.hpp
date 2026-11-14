#pragma once
// Demo strategy: top-of-book imbalance taker. Not expected to make money; it exists
// to give the engine a realistic decide step and a reason to send orders.
//
// When the touch is one tick wide and one side's displayed size dominates
// (imbalance beyond a threshold), send an IOC at the far touch in the direction of
// the heavier side, within a position limit. Integer math only, no branches on
// data that can't be predicted except the final "act" decision.
#include <cstdint>

#include "lle/common.hpp"
#include "lle/ladder.hpp"

namespace lle {

struct StrategyParams {
  // Act when |bid - ask| / (bid + ask) >= num/den.
  std::int64_t imbalance_num = 6;
  std::int64_t imbalance_den = 10;
  std::int64_t max_spread_ticks = 1;
  std::int64_t order_qty = 10;
  std::int64_t position_limit = 200;
  // Minimum cycles between orders per instrument (avoids firing on every message
  // while the book stays imbalanced).
  std::uint64_t cooldown_cycles = 0;
};

struct Decision {
  bool act = false;
  Side side = Side::Buy;
  std::int64_t price = 0;
  std::int64_t qty = 0;
};

class ImbalanceStrategy {
 public:
  explicit ImbalanceStrategy(const StrategyParams& p = {}) : p_(p) {}

  LLE_ALWAYS_INLINE Decision decide(std::int64_t bid, std::int64_t bid_qty, std::int64_t ask, std::int64_t ask_qty,
                                    std::int64_t position, std::uint64_t now_tsc, std::uint64_t& last_tsc) const noexcept {
    Decision d;
    if (LLE_UNLIKELY(bid == BidLadder::kNone || ask == AskLadder::kNone)) return d;
    const std::int64_t spread = ask - bid;
    if (spread <= 0 || spread > p_.max_spread_ticks) return d;
    const std::int64_t tot = bid_qty + ask_qty;
    if (tot <= 0) return d;
    const std::int64_t diff = bid_qty - ask_qty;
    const std::int64_t lhs = (diff < 0 ? -diff : diff) * p_.imbalance_den;
    const std::int64_t rhs = tot * p_.imbalance_num;
    if (lhs < rhs) return d;
    if (now_tsc - last_tsc < p_.cooldown_cycles) return d;
    const bool buy = diff > 0;  // more resting bids: price pressure up, lift the offer
    if (buy ? position + p_.order_qty > p_.position_limit : position - p_.order_qty < -p_.position_limit) return d;
    d.act = true;
    d.side = buy ? Side::Buy : Side::Sell;
    d.price = buy ? ask : bid;
    d.qty = p_.order_qty;
    last_tsc = now_tsc;
    return d;
  }

  const StrategyParams& params() const noexcept { return p_; }
  void set_params(const StrategyParams& p) noexcept { p_ = p; }

 private:
  StrategyParams p_;
};

}  // namespace lle
