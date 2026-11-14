#pragma once
// Order-by-order market book for a handful of tracked instruments (ITCH-style feeds).
// Each instrument has a flat bid ladder and ask ladder; every live order sits in a
// preallocated pool, found by order reference through OrderIdMap.
//
// Prices are kept in ticks of $0.01. ITCH prices carry 4 implied decimals, so the
// divide-by-100 is a constant divisor (multiply + shift). Sub-penny prices (stocks
// under $1) are not supported; such symbols should not be tracked.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "lle/common.hpp"
#include "lle/ladder.hpp"
#include "lle/order_map.hpp"

namespace lle {

struct alignas(16) BookOrder {
  std::int64_t price;   // ticks
  std::uint32_t qty;
  std::uint16_t book;
  std::uint8_t side;    // 0 buy, 1 sell
  std::uint8_t in_ladder;
};
static_assert(sizeof(BookOrder) == 16);

struct InstrumentBook {
  explicit InstrumentBook(std::uint32_t levels_log2) : bids(levels_log2), asks(levels_log2) {}
  BidLadder bids;
  AskLadder asks;
  std::uint64_t updates = 0;
  std::uint64_t dropped_out_of_window = 0;
};

// Result of a book update, used to decide whether the strategy needs to look.
struct BookEvent {
  std::int16_t book = -1;  // -1: not a tracked instrument / unknown order
  bool top_changed = false;
};

struct BookConfig {
  std::uint32_t levels_log2 = 15;          // 32768 one-cent levels = $327 window per side
  std::uint32_t order_map_log2 = 22;       // 4M slots
  std::uint32_t order_pool = 3u << 20;     // 3M live orders
  std::uint32_t price_scale = 100;         // raw feed units per tick
};

class MarketBook {
 public:
  explicit MarketBook(const BookConfig& cfg = {})
      : cfg_(cfg), ids_(cfg.order_map_log2), pool_(cfg.order_pool) {
    locate_to_book_.fill(-1);
  }

  // Registers an instrument by feed locate code. Returns the book index.
  int add_instrument(std::uint16_t locate) {
    books_.push_back(std::make_unique<InstrumentBook>(cfg_.levels_log2));
    const int idx = static_cast<int>(books_.size() - 1);
    locate_to_book_[locate] = static_cast<std::int16_t>(idx);
    return idx;
  }
  LLE_ALWAYS_INLINE int book_for(std::uint16_t locate) const noexcept { return locate_to_book_[locate]; }
  InstrumentBook& book(int i) noexcept { return *books_[i]; }
  const InstrumentBook& book(int i) const noexcept { return *books_[i]; }
  std::size_t num_books() const noexcept { return books_.size(); }

  LLE_ALWAYS_INLINE std::int64_t to_ticks(std::uint32_t raw) const noexcept {
    // Compile-time divisor when price_scale is the default.
    return cfg_.price_scale == 100 ? static_cast<std::int64_t>(raw / 100u)
                                   : static_cast<std::int64_t>(raw / cfg_.price_scale);
  }

  LLE_ALWAYS_INLINE BookEvent add(std::uint64_t ref, std::uint16_t locate, Side side, std::uint32_t raw_price,
                                  std::uint32_t qty) noexcept {
    const int b = book_for(locate);
    if (b < 0) return {};
    InstrumentBook& ib = *books_[b];
    const std::uint32_t slot = pool_.alloc();
    if (LLE_UNLIKELY(slot == Pool<BookOrder>::kNone)) { pool_exhausted_++; return {}; }
    const std::int64_t px = to_ticks(raw_price);
    bool in = false, top = false;
    if (side == Side::Buy) {
      const std::int64_t before = ib.bids.best_price();
      in = ib.bids.add(px, qty);
      top = in && px >= before;
    } else {
      const std::int64_t before = ib.asks.best_price();
      in = ib.asks.add(px, qty);
      top = in && (before == AskLadder::kNone || px <= before);
    }
    if (!in) ib.dropped_out_of_window++;
    pool_[slot] = BookOrder{px, qty, static_cast<std::uint16_t>(b), static_cast<std::uint8_t>(side),
                            static_cast<std::uint8_t>(in)};
    if (LLE_UNLIKELY(!ids_.insert(ref, slot))) { map_full_++; pool_.release(slot); return {}; }
    ib.updates++;
    return {static_cast<std::int16_t>(b), top};
  }

  // Removes `qty` shares from an order (execution or partial cancel). If the order
  // reaches zero it is deleted. `exec_price` receives the order's price in ticks.
  LLE_ALWAYS_INLINE BookEvent reduce(std::uint64_t ref, std::uint32_t qty, std::int64_t* exec_price = nullptr,
                                     Side* exec_side = nullptr) noexcept {
    const std::uint32_t slot = ids_.find(ref);
    if (slot == OrderIdMap::kEmpty) return {};
    BookOrder& o = pool_[slot];
    const std::uint32_t take = qty < o.qty ? qty : o.qty;
    if (exec_price) *exec_price = o.price;
    if (exec_side) *exec_side = static_cast<Side>(o.side);
    const bool last = take == o.qty;
    const bool top = remove_from_ladder(o, take, last);
    o.qty -= take;
    const std::int16_t b = static_cast<std::int16_t>(o.book);
    books_[b]->updates++;
    if (last) {
      ids_.erase(ref);
      pool_.release(slot);
    }
    return {b, top};
  }

  LLE_ALWAYS_INLINE BookEvent remove(std::uint64_t ref) noexcept {
    const std::uint32_t slot = ids_.find(ref);
    if (slot == OrderIdMap::kEmpty) return {};
    BookOrder& o = pool_[slot];
    const bool top = remove_from_ladder(o, o.qty, true);
    const std::int16_t b = static_cast<std::int16_t>(o.book);
    books_[b]->updates++;
    ids_.erase(ref);
    pool_.release(slot);
    return {b, top};
  }

  // ITCH 'U': the old order is removed and a new one with a new ref takes its place
  // (losing time priority) on the same side and instrument.
  LLE_ALWAYS_INLINE BookEvent replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t qty,
                                      std::uint32_t raw_price) noexcept {
    const std::uint32_t slot = ids_.find(old_ref);
    if (slot == OrderIdMap::kEmpty) return {};
    BookOrder& o = pool_[slot];
    const bool top1 = remove_from_ladder(o, o.qty, true);
    InstrumentBook& ib = *books_[o.book];
    const std::int64_t px = to_ticks(raw_price);
    bool in, top2;
    if (o.side == 0) {
      const std::int64_t before = ib.bids.best_price();
      in = ib.bids.add(px, qty);
      top2 = in && px >= before;
    } else {
      const std::int64_t before = ib.asks.best_price();
      in = ib.asks.add(px, qty);
      top2 = in && (before == AskLadder::kNone || px <= before);
    }
    o.price = px;
    o.qty = qty;
    o.in_ladder = in;
    ids_.erase(old_ref);
    ids_.insert(new_ref, slot);
    ib.updates++;
    return {static_cast<std::int16_t>(o.book), top1 || top2};
  }

  std::uint64_t digest() const noexcept {
    std::uint64_t h = 0;
    for (const auto& b : books_) h = h * 31 + (b->bids.digest() ^ (b->asks.digest() << 1));
    return h;
  }
  std::uint64_t live_orders() const noexcept { return ids_.size(); }
  std::uint64_t pool_exhausted() const noexcept { return pool_exhausted_; }

 private:
  // Returns true if the touch on that side changed.
  LLE_ALWAYS_INLINE bool remove_from_ladder(const BookOrder& o, std::uint32_t qty, bool last) noexcept {
    if (!o.in_ladder) return false;
    InstrumentBook& ib = *books_[o.book];
    if (o.side == 0) {
      const bool at_top = o.price == ib.bids.best_price();
      ib.bids.reduce(o.price, qty, last ? 1 : 0);
      return at_top;
    }
    const bool at_top = o.price == ib.asks.best_price();
    ib.asks.reduce(o.price, qty, last ? 1 : 0);
    return at_top;
  }

  BookConfig cfg_;
  std::array<std::int16_t, 65536> locate_to_book_;
  std::vector<std::unique_ptr<InstrumentBook>> books_;
  OrderIdMap ids_;
  Pool<BookOrder> pool_;
  std::uint64_t pool_exhausted_ = 0;
  std::uint64_t map_full_ = 0;
};

}  // namespace lle
