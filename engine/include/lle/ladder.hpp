#pragma once
// One side of a price-level book stored as a flat array indexed by price tick.
//  - Level is 16 bytes, so four adjacent prices share a cache line; the touch and
//    the next few levels are almost always in L1.
//  - A two-level bitmap (one bit per level, one bit per 64-level word) finds the
//    next non-empty level with tzcnt/lzcnt instead of walking empty levels.
//  - No allocation after construction (recentering, a cold path, allocates once).
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lle/common.hpp"
#include "lle/memory.hpp"

namespace lle {

struct Level {
  std::int64_t qty;
  std::uint32_t orders;
  std::uint32_t pad;
};
static_assert(sizeof(Level) == 16);

template <bool IsBid>
class PriceLadder {
 public:
  static constexpr std::int64_t kNone = INT64_MIN;

  explicit PriceLadder(std::uint32_t levels_log2 = 15) : n_(1u << levels_log2) {
    assert(levels_log2 >= 12 && levels_log2 <= 24);
    levels_ = static_cast<Level*>(table_alloc(sizeof(Level) * n_));
    words_ = static_cast<std::uint64_t*>(std::aligned_alloc(kCacheLine, bytes_rounded(nwords() * 8)));
    summary_ = static_cast<std::uint64_t*>(std::aligned_alloc(kCacheLine, bytes_rounded(nsum() * 8)));
    clear();
  }
  ~PriceLadder() {
    table_free(levels_, sizeof(Level) * n_);
    std::free(words_);
    std::free(summary_);
  }
  PriceLadder(const PriceLadder&) = delete;
  PriceLadder& operator=(const PriceLadder&) = delete;

  void clear() noexcept {
    std::memset(levels_, 0, sizeof(Level) * n_);
    std::memset(words_, 0, nwords() * 8);
    std::memset(summary_, 0, nsum() * 8);
    best_ = -1;
    based_ = false;
  }

  // Centers the window on `price` the first time a price is seen.
  LLE_ALWAYS_INLINE void ensure_base(std::int64_t price) noexcept {
    if (LLE_UNLIKELY(!based_)) {
      base_ = price - static_cast<std::int64_t>(n_ / 2);
      based_ = true;
    }
  }
  bool based() const noexcept { return based_; }
  std::int64_t base() const noexcept { return base_; }
  std::uint32_t capacity() const noexcept { return n_; }

  LLE_ALWAYS_INLINE bool in_window(std::int64_t price) const noexcept {
    return based_ && static_cast<std::uint64_t>(price - base_) < n_;
  }

  // Adds qty at price. Returns false if the price is outside the window (dropped).
  LLE_ALWAYS_INLINE bool add(std::int64_t price, std::int64_t qty, std::uint32_t norders = 1) noexcept {
    ensure_base(price);
    const std::uint64_t i = static_cast<std::uint64_t>(price - base_);
    if (LLE_UNLIKELY(i >= n_)) return false;
    Level& l = levels_[i];
    if (l.qty == 0) set_bit(static_cast<std::uint32_t>(i));
    l.qty += qty;
    l.orders += norders;
    if (better_or_none(static_cast<std::int32_t>(i))) best_ = static_cast<std::int32_t>(i);
    return true;
  }

  // Removes qty at price; clears the level when it reaches zero.
  LLE_ALWAYS_INLINE void reduce(std::int64_t price, std::int64_t qty, std::uint32_t norders = 0) noexcept {
    const std::uint64_t i = static_cast<std::uint64_t>(price - base_);
    if (LLE_UNLIKELY(i >= n_)) return;
    Level& l = levels_[i];
    if (l.qty == 0) return;
    l.qty -= qty;
    l.orders = l.orders >= norders ? l.orders - norders : 0;
    if (l.qty <= 0) empty_level(static_cast<std::uint32_t>(i));
  }

  // Sets the absolute quantity at a level (L2 feeds). qty == 0 deletes. Recenters
  // the window if a price near the touch arrives outside it (rare, cold path).
  LLE_ALWAYS_INLINE void set(std::int64_t price, std::int64_t qty) noexcept {
    ensure_base(price);
    std::uint64_t i = static_cast<std::uint64_t>(price - base_);
    if (LLE_UNLIKELY(i >= n_)) {
      if (qty == 0) return;
      const std::int64_t d = best_ >= 0 ? price - best_price() : 0;
      if (d > static_cast<std::int64_t>(n_ / 4) || -d > static_cast<std::int64_t>(n_ / 4)) return;
      recenter(price);
      i = static_cast<std::uint64_t>(price - base_);
    }
    Level& l = levels_[i];
    if (qty == 0) {
      if (l.qty != 0) empty_level(static_cast<std::uint32_t>(i));
      return;
    }
    if (l.qty == 0) set_bit(static_cast<std::uint32_t>(i));
    l.qty = qty;
    l.orders = 1;
    if (better_or_none(static_cast<std::int32_t>(i))) best_ = static_cast<std::int32_t>(i);
  }

  LLE_ALWAYS_INLINE bool empty() const noexcept { return best_ < 0; }
  LLE_ALWAYS_INLINE std::int64_t best_price() const noexcept { return best_ < 0 ? kNone : base_ + best_; }
  LLE_ALWAYS_INLINE std::int64_t best_qty() const noexcept { return best_ < 0 ? 0 : levels_[best_].qty; }
  std::int64_t qty_at(std::int64_t price) const noexcept {
    const std::uint64_t i = static_cast<std::uint64_t>(price - base_);
    return (based_ && i < n_) ? levels_[i].qty : 0;
  }
  std::uint32_t orders_at(std::int64_t price) const noexcept {
    const std::uint64_t i = static_cast<std::uint64_t>(price - base_);
    return (based_ && i < n_) ? levels_[i].orders : 0;
  }

  // Next non-empty price strictly worse than `price`, or kNone.
  std::int64_t next_worse(std::int64_t price) const noexcept {
    const std::int64_t i = price - base_;
    if (i < 0) return IsBid ? kNone : best_price();
    if (i >= static_cast<std::int64_t>(n_)) return IsBid ? best_price() : kNone;
    const std::int32_t j =
        IsBid ? find_prev(static_cast<std::int32_t>(i) - 1) : find_next(static_cast<std::int32_t>(i) + 1);
    return j < 0 ? kNone : base_ + j;
  }

  // Visits up to `depth` levels from the touch outward: f(price, qty, orders).
  template <class F>
  int for_each_level(int depth, F&& f) const {
    int k = 0;
    for (std::int32_t i = best_; i >= 0 && k < depth; ++k) {
      f(base_ + i, levels_[i].qty, levels_[i].orders);
      i = IsBid ? find_prev(i - 1) : find_next(i + 1);
    }
    return k;
  }

  // Digest of the whole side, for deterministic-replay checks.
  std::uint64_t digest() const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::int32_t i = best_; i >= 0; i = IsBid ? find_prev(i - 1) : find_next(i + 1)) {
      h = (h ^ static_cast<std::uint64_t>(base_ + i)) * 1099511628211ull;
      h = (h ^ static_cast<std::uint64_t>(levels_[i].qty)) * 1099511628211ull;
    }
    return h;
  }

  std::uint64_t recenters() const noexcept { return recenters_; }

 private:
  static std::size_t bytes_rounded(std::size_t b) { return ((b + 63) / 64) * 64; }
  std::uint32_t nwords() const noexcept { return n_ / 64; }
  std::uint32_t nsum() const noexcept { return (nwords() + 63) / 64; }

  LLE_ALWAYS_INLINE bool better_or_none(std::int32_t i) const noexcept {
    return best_ < 0 || (IsBid ? i > best_ : i < best_);
  }
  LLE_ALWAYS_INLINE void set_bit(std::uint32_t i) noexcept {
    words_[i >> 6] |= 1ull << (i & 63);
    summary_[i >> 12] |= 1ull << ((i >> 6) & 63);
  }
  LLE_ALWAYS_INLINE void clear_bit(std::uint32_t i) noexcept {
    std::uint64_t& w = words_[i >> 6];
    w &= ~(1ull << (i & 63));
    if (w == 0) summary_[i >> 12] &= ~(1ull << ((i >> 6) & 63));
  }
  LLE_ALWAYS_INLINE void empty_level(std::uint32_t i) noexcept {
    levels_[i].qty = 0;
    levels_[i].orders = 0;
    clear_bit(i);
    if (static_cast<std::int32_t>(i) == best_)
      best_ = IsBid ? find_prev(static_cast<std::int32_t>(i) - 1) : find_next(static_cast<std::int32_t>(i) + 1);
  }

  // Highest set index <= i, or -1.
  std::int32_t find_prev(std::int32_t i) const noexcept {
    if (i < 0) return -1;
    if (i >= static_cast<std::int32_t>(n_)) i = static_cast<std::int32_t>(n_) - 1;
    const std::int32_t w = i >> 6;
    const std::uint64_t bits = words_[w] & (~0ull >> (63 - (i & 63)));
    if (bits) return (w << 6) + 63 - std::countl_zero(bits);
    std::int32_t s = w >> 6;
    const int wb = w & 63;  // words strictly below w within summary word s
    std::uint64_t sb = wb ? (summary_[s] & (~0ull >> (64 - wb))) : 0;
    while (true) {
      if (sb) {
        const std::int32_t ww = (s << 6) + 63 - std::countl_zero(sb);
        return (ww << 6) + 63 - std::countl_zero(words_[ww]);
      }
      if (--s < 0) return -1;
      sb = summary_[s];
    }
  }
  // Lowest set index >= i, or -1.
  std::int32_t find_next(std::int32_t i) const noexcept {
    if (i < 0) i = 0;
    if (i >= static_cast<std::int32_t>(n_)) return -1;
    const std::int32_t w = i >> 6;
    const std::uint64_t bits = words_[w] & (~0ull << (i & 63));
    if (bits) return (w << 6) + std::countr_zero(bits);
    std::int32_t s = w >> 6;
    const std::int32_t ns = static_cast<std::int32_t>(nsum());
    const int wb = w & 63;  // words strictly above w within summary word s
    std::uint64_t sb = (wb == 63) ? 0 : (summary_[s] & (~0ull << (wb + 1)));
    while (true) {
      if (sb) {
        const std::int32_t ww = (s << 6) + std::countr_zero(sb);
        return (ww << 6) + std::countr_zero(words_[ww]);
      }
      if (++s >= ns) return -1;
      sb = summary_[s];
    }
  }

  LLE_COLD void recenter(std::int64_t price) noexcept {
    const std::int64_t new_base = price - static_cast<std::int64_t>(n_ / 2);
    const std::int64_t shift = new_base - base_;
    auto* tmp = static_cast<Level*>(std::aligned_alloc(kCacheLine, sizeof(Level) * n_));
    std::memset(tmp, 0, sizeof(Level) * n_);
    for (std::uint32_t i = 0; i < n_; ++i) {
      if (levels_[i].qty == 0) continue;
      const std::int64_t j = static_cast<std::int64_t>(i) - shift;
      if (j >= 0 && j < static_cast<std::int64_t>(n_)) tmp[j] = levels_[i];
    }
    std::memcpy(levels_, tmp, sizeof(Level) * n_);
    std::free(tmp);
    std::memset(words_, 0, nwords() * 8);
    std::memset(summary_, 0, nsum() * 8);
    best_ = -1;
    base_ = new_base;
    for (std::uint32_t i = 0; i < n_; ++i) {
      if (levels_[i].qty == 0) continue;
      set_bit(i);
      if (better_or_none(static_cast<std::int32_t>(i))) best_ = static_cast<std::int32_t>(i);
    }
    recenters_++;
  }

  std::uint32_t n_;
  Level* levels_ = nullptr;
  std::uint64_t* words_ = nullptr;
  std::uint64_t* summary_ = nullptr;
  std::int64_t base_ = 0;
  std::int32_t best_ = -1;
  bool based_ = false;
  std::uint64_t recenters_ = 0;
};

using BidLadder = PriceLadder<true>;
using AskLadder = PriceLadder<false>;

}  // namespace lle
