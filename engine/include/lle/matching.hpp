#pragma once
// Price-time priority matching engine for one instrument.
//  - Flat price ladders (shared with the market-data book) hold aggregate qty and
//    the non-empty bitmap; a parallel array holds each level's FIFO head/tail.
//  - Orders live in a preallocated pool and form intrusive doubly linked lists, so
//    cancel is O(1) and a fill touches only the head of the queue.
//  - Events go to a handler object; nothing is allocated after construction.
//
// Each resting order also carries `queue_ahead`, the displayed size that was in
// front of it on the real market when it arrived. The paper exchange uses it for
// queue-position fills; plain matching ignores it.
#include <cstdint>
#include <cstring>

#include "lle/common.hpp"
#include "lle/ladder.hpp"
#include "lle/memory.hpp"
#include "lle/order_map.hpp"

namespace lle {

struct MeOrder {
  std::uint64_t id;
  std::uint64_t owner;
  std::int64_t price;
  std::int64_t queue_ahead;
  std::uint32_t qty;
  std::uint32_t prev, next;
  std::uint8_t side;
  std::uint8_t pad[3];
};

enum class Tif : std::uint8_t { Day, Ioc };

struct NullMatchHandler {
  void on_fill(const MeOrder& maker, std::uint64_t taker_id, std::uint64_t taker_owner, std::int64_t px,
               std::uint32_t qty) {}
  void on_rest(const MeOrder&) {}
  void on_done(std::uint64_t id, std::uint32_t leftover) {}  // IOC remainder / cancel
  void on_reject(std::uint64_t id, char reason) {}
};

class MatchingEngine {
 public:
  static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

  MatchingEngine(std::uint32_t levels_log2, std::uint32_t max_orders, std::uint32_t map_log2)
      : bids_(levels_log2), asks_(levels_log2), nlev_(1u << levels_log2), ids_(map_log2, OrderIdMap::Hash::Fibonacci),
        pool_(max_orders) {
    bfifo_ = static_cast<Fifo*>(table_alloc(sizeof(Fifo) * nlev_));
    afifo_ = static_cast<Fifo*>(table_alloc(sizeof(Fifo) * nlev_));
    for (std::uint32_t i = 0; i < nlev_; ++i) bfifo_[i] = afifo_[i] = Fifo{kNil, kNil};
  }
  ~MatchingEngine() {
    table_free(bfifo_, sizeof(Fifo) * nlev_);
    table_free(afifo_, sizeof(Fifo) * nlev_);
  }
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  // Fixes the price window around `center` (call once before the first order).
  void set_center(std::int64_t center) {
    bids_.ensure_base(center);
    asks_.ensure_base(center);
  }

  // Submits an order; matches against resting orders, then rests (Day) or
  // expires (IOC). reason codes: 'D' duplicate id, 'P' price outside window,
  // 'F' book full, 'Q' bad qty.
  template <class H>
  void submit(std::uint64_t id, std::uint64_t owner, Side side, std::int64_t price, std::uint32_t qty, Tif tif,
              std::int64_t queue_ahead, H& h) {
    if (qty == 0) { h.on_reject(id, 'Q'); return; }
    if (ids_.find(id) != OrderIdMap::kEmpty) { h.on_reject(id, 'D'); return; }
    set_center(price);
    if (!bids_.in_window(price)) { h.on_reject(id, 'P'); return; }
    std::uint32_t left = qty;
    if (side == Side::Buy) left = match<false>(id, owner, price, left, h);
    else left = match<true>(id, owner, price, left, h);
    if (left == 0) { h.on_done(id, 0); return; }
    if (tif == Tif::Ioc) { h.on_done(id, left); return; }
    const std::uint32_t slot = pool_.alloc();
    if (slot == Pool<MeOrder>::kNone) { h.on_reject(id, 'F'); return; }
    MeOrder& o = pool_[slot];
    o = MeOrder{id, owner, price, queue_ahead, left, kNil, kNil, static_cast<std::uint8_t>(side), {}};
    ids_.insert(id, slot);
    Fifo& f = fifo(side, price);
    o.prev = f.tail;
    if (f.tail != kNil) pool_[f.tail].next = slot; else f.head = slot;
    f.tail = slot;
    if (side == Side::Buy) bids_.add(price, left); else asks_.add(price, left);
    h.on_rest(o);
  }

  template <class H>
  bool cancel(std::uint64_t id, H& h) {
    const std::uint32_t slot = ids_.find(id);
    if (slot == OrderIdMap::kEmpty) return false;
    const std::uint32_t left = pool_[slot].qty;
    unlink(slot);
    h.on_done(id, left);
    return true;
  }

  // Fills `qty` of a specific resting order from outside the book (the paper
  // exchange does this when the real market trades through our price).
  template <class H>
  std::uint32_t fill_resting(std::uint32_t slot, std::uint32_t qty, std::int64_t px, H& h) {
    MeOrder& o = pool_[slot];
    const std::uint32_t take = qty < o.qty ? qty : o.qty;
    if (take == 0) return 0;
    h.on_fill(o, 0, 0, px, take);
    o.qty -= take;
    if (o.side == 0) bids_.reduce(o.price, take); else asks_.reduce(o.price, take);
    if (o.qty == 0) {
      const std::uint64_t oid = o.id;
      unlink_no_ladder(slot);
      h.on_done(oid, 0);
    }
    return take;
  }

  // Iterates resting orders at a price in FIFO order: f(slot, order&).
  template <class F>
  void for_each_at(Side side, std::int64_t price, F&& f) {
    if (!bids_.in_window(price)) return;
    for (std::uint32_t s = fifo(side, price).head; s != kNil;) {
      const std::uint32_t nx = pool_[s].next;
      f(s, pool_[s]);
      s = nx;
    }
  }

  const BidLadder& bids() const noexcept { return bids_; }
  const AskLadder& asks() const noexcept { return asks_; }
  std::uint32_t resting() const noexcept { return pool_.used(); }
  MeOrder* find(std::uint64_t id) noexcept {
    const std::uint32_t s = ids_.find(id);
    return s == OrderIdMap::kEmpty ? nullptr : &pool_[s];
  }
  std::uint32_t slot_of(std::uint64_t id) const noexcept { return ids_.find(id); }

 private:
  struct Fifo {
    std::uint32_t head, tail;
  };
  Fifo& fifo(Side s, std::int64_t price) noexcept {
    return s == Side::Buy ? bfifo_[price - bids_.base()] : afifo_[price - asks_.base()];
  }

  // Taker matches against the opposite side. kAgainstBids: incoming sell.
  template <bool kAgainstBids, class H>
  std::uint32_t match(std::uint64_t id, std::uint64_t owner, std::int64_t limit, std::uint32_t left, H& h) {
    while (left) {
      std::int64_t best;
      if constexpr (kAgainstBids) {
        best = bids_.best_price();
        if (best == BidLadder::kNone || best < limit) break;
      } else {
        best = asks_.best_price();
        if (best == AskLadder::kNone || best > limit) break;
      }
      Fifo& f = kAgainstBids ? bfifo_[best - bids_.base()] : afifo_[best - asks_.base()];
      while (left && f.head != kNil) {
        const std::uint32_t s = f.head;
        MeOrder& m = pool_[s];
        const std::uint32_t take = left < m.qty ? left : m.qty;
        h.on_fill(m, id, owner, best, take);
        m.qty -= take;
        left -= take;
        if constexpr (kAgainstBids) bids_.reduce(best, take); else asks_.reduce(best, take);
        if (m.qty == 0) {
          const std::uint64_t mid = m.id;
          unlink_no_ladder(s);
          h.on_done(mid, 0);
        }
      }
    }
    return left;
  }

  void unlink(std::uint32_t slot) noexcept {
    MeOrder& o = pool_[slot];
    if (o.side == 0) bids_.reduce(o.price, o.qty); else asks_.reduce(o.price, o.qty);
    unlink_no_ladder(slot);
  }
  void unlink_no_ladder(std::uint32_t slot) noexcept {
    MeOrder& o = pool_[slot];
    Fifo& f = fifo(static_cast<Side>(o.side), o.price);
    if (o.prev != kNil) pool_[o.prev].next = o.next; else f.head = o.next;
    if (o.next != kNil) pool_[o.next].prev = o.prev; else f.tail = o.prev;
    ids_.erase(o.id);
    pool_.release(slot);
  }

  BidLadder bids_;
  AskLadder asks_;
  std::uint32_t nlev_;
  Fifo* bfifo_ = nullptr;
  Fifo* afifo_ = nullptr;
  OrderIdMap ids_;
  Pool<MeOrder> pool_;
};

}  // namespace lle
