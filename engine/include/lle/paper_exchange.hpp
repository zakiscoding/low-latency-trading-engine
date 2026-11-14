#pragma once
// Simulated exchange for paper trading against a live (or replayed) market.
//
// It never routes anything anywhere: orders are filled against a mirror of the
// real book with these rules.
//  1. An order takes effect `latency_ns` after it was received (gateway + wire).
//  2. The marketable part walks the displayed opposite side, minus liquidity this
//     simulator already took at that level since the level last changed. Taker fee.
//  3. The rest is cancelled (IOC) or rests (Day) with queue_ahead = displayed size
//     at that price on our side when it arrived: we join the back of the queue.
//  4. Real executions at our price on our side burn queue_ahead first; once it is
//     gone, further executed size fills us. Maker fee (negative = rebate).
//  5. A trade printing through our price (e.g. a sell executes below our bid), or
//     the opposite touch crossing our price, fills us: we'd have been hit first.
//  6. Cancels ahead of us: queue_ahead = min(queue_ahead, displayed level size).
// Known optimism: our own fills don't remove real liquidity from later real
// events, and hidden/iceberg liquidity isn't modelled.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "lle/book.hpp"
#include "lle/common.hpp"
#include "lle/itch.hpp"
#include "lle/matching.hpp"
#include "lle/order_codec.hpp"

namespace lle {

struct FeeModel {
  enum Kind : std::uint8_t { PerUnit, Bps } kind = PerUnit;
  // PerUnit: micro-dollars per unit of qty (Nasdaq: taker 3000 = $0.0030/share).
  // Bps: hundredths of a basis point of notional (Coinbase: 6000 = 0.60%).
  std::int64_t maker = -2000;
  std::int64_t taker = 3000;
};

struct PaperConfig {
  std::int64_t latency_ns = 20'000;     // simulated order-entry latency
  std::int64_t tick_micros = 10'000;    // one price tick in micro-dollars ($0.01)
  std::int64_t qty_scale = 1;           // qty units per whole unit (1 share, or 1e8 for BTC)
  FeeModel fees{};
  std::int64_t max_order_qty = 1'000'000;
  std::uint32_t levels_log2 = 15;
};

struct PaperAccount {
  std::int64_t cash_micros = 0;
  std::int64_t fees_micros = 0;
  std::array<std::int64_t, 64> position{};
  std::uint64_t orders = 0, fills = 0, maker_fills = 0, taker_fills = 0, rejects = 0;
  std::int64_t volume_micros = 0;
};

class PaperExchange {
 public:
  using ReportFn = void (*)(void* ctx, const ExecReport&, int instrument);

  PaperExchange(const PaperConfig& cfg, int instruments, ReportFn fn, void* ctx)
      : cfg_(cfg), report_(fn), ctx_(ctx) {
    for (int i = 0; i < instruments; ++i) {
      inst_.push_back(std::make_unique<Instrument>(cfg.levels_log2));
    }
    pending_.resize(4096);
  }

  // ---------- market data (mirror book) ----------
  BidLadder& mkt_bids(int i) { return inst_[i]->mbids; }
  AskLadder& mkt_asks(int i) { return inst_[i]->masks; }

  // Absolute displayed size at a level changed (L2 feeds, or derived from L3).
  void on_market_level(int i, Side side, std::int64_t price, std::int64_t qty) {
    Instrument& in = *inst_[i];
    if (side == Side::Buy) in.mbids.set(price, qty); else in.masks.set(price, qty);
    in.taken[static_cast<int>(side)].erase_level(price);
    after_level_change(i, side, price);
  }

  // A real trade printed. `resting_side` is the side of the order that was hit.
  void on_market_trade(int i, Side resting_side, std::int64_t price, std::int64_t qty) {
    Instrument& in = *inst_[i];
    MatchingEngine& me = in.ours;
    Handler h{this, i, true};
    if (resting_side == Side::Buy) {
      // Our bids at prices above the trade price would have been hit first.
      for (std::int64_t p = me.bids().best_price(); p != BidLadder::kNone && p > price; p = me.bids().best_price())
        fill_level(i, Side::Buy, p, INT64_MAX, p, h);
      fill_level(i, Side::Buy, price, qty, price, h);
    } else {
      for (std::int64_t p = me.asks().best_price(); p != AskLadder::kNone && p < price; p = me.asks().best_price())
        fill_level(i, Side::Sell, p, INT64_MAX, p, h);
      fill_level(i, Side::Sell, price, qty, price, h);
    }
  }

  // ---------- order entry ----------
  // Accepts a 64-byte order message received at `now_ns`.
  void submit(const OrderMsgView& m, std::int64_t now_ns) {
    Pending& p = pending_[(head_ + count_) % pending_.size()];
    if (count_ == pending_.size()) {
      reject(m.token(), m.side(), 'B', static_cast<int>(m.instrument()));
      return;
    }
    std::memcpy(p.msg, m.p, kOrderMsgSize);
    p.due_ns = now_ns + cfg_.latency_ns;
    count_++;
  }

  // Applies orders whose latency has elapsed. Call often (every loop iteration).
  void tick(std::int64_t now_ns) {
    while (count_ && pending_[head_].due_ns <= now_ns) {
      apply(OrderMsgView{pending_[head_].msg});
      head_ = (head_ + 1) % pending_.size();
      count_--;
    }
  }
  std::size_t pending() const { return count_; }

  const PaperAccount& account() const { return acct_; }
  // Mark-to-market P&L in micro-dollars using mid prices.
  std::int64_t pnl_micros() const {
    std::int64_t v = acct_.cash_micros;
    for (std::size_t i = 0; i < inst_.size(); ++i) {
      const auto& in = *inst_[i];
      if (!acct_.position[i]) continue;
      const std::int64_t b = in.mbids.best_price(), a = in.masks.best_price();
      if (b == BidLadder::kNone || a == AskLadder::kNone) continue;
      v += notional(acct_.position[i], (a + b) / 2);
    }
    return v;
  }
  std::uint32_t resting(int i) const { return inst_[i]->ours.resting(); }

  std::int64_t notional(std::int64_t qty, std::int64_t price_ticks) const {
    const __int128 n = static_cast<__int128>(price_ticks) * cfg_.tick_micros * qty / cfg_.qty_scale;
    return static_cast<std::int64_t>(n);
  }

 private:
  // Liquidity this simulator consumed per displayed level, cleared on level change.
  struct Taken {
    std::vector<std::pair<std::int64_t, std::int64_t>> v;  // (price, qty), tiny
    std::int64_t get(std::int64_t p) const {
      for (auto& e : v) if (e.first == p) return e.second;
      return 0;
    }
    void add(std::int64_t p, std::int64_t q) {
      for (auto& e : v) if (e.first == p) { e.second += q; return; }
      v.emplace_back(p, q);
    }
    void erase_level(std::int64_t p) {
      for (std::size_t k = 0; k < v.size(); ++k)
        if (v[k].first == p) { v[k] = v.back(); v.pop_back(); return; }
    }
  };
  struct Instrument {
    explicit Instrument(std::uint32_t l2) : mbids(l2), masks(l2), ours(l2, 1u << 16, 17) {}
    BidLadder mbids;
    AskLadder masks;
    MatchingEngine ours;
    std::array<Taken, 2> taken;
  };
  struct Pending {
    unsigned char msg[kOrderMsgSize];
    std::int64_t due_ns;
  };
  struct Handler {
    PaperExchange* x;
    int inst;
    bool maker;
    void on_fill(const MeOrder& m, std::uint64_t, std::uint64_t, std::int64_t px, std::uint32_t q) {
      x->book_fill(inst, static_cast<Side>(m.side), m.id, px, q, true);
    }
    void on_rest(const MeOrder& m) {
      x->report_(x->ctx_, ExecReport{'A', static_cast<Side>(m.side), 'M', 0, m.qty, m.id, m.price, 0}, inst);
    }
    void on_done(std::uint64_t id, std::uint32_t left) {
      if (left) x->report_(x->ctx_, ExecReport{'C', Side::Buy, ' ', 0, left, id, 0, 0}, inst);
    }
    void on_reject(std::uint64_t id, char r) { x->reject(id, Side::Buy, r, inst); }
  };

  void reject(std::uint64_t token, Side s, char reason, int inst) {
    acct_.rejects++;
    report_(ctx_, ExecReport{'R', s, ' ', reason, 0, token, 0, 0}, inst);
  }

  void book_fill(int inst, Side side, std::uint64_t token, std::int64_t px, std::int64_t qty, bool maker) {
    const std::int64_t n = notional(qty, px);
    std::int64_t fee;
    if (cfg_.fees.kind == FeeModel::PerUnit) fee = (maker ? cfg_.fees.maker : cfg_.fees.taker) * qty / cfg_.qty_scale;
    else fee = static_cast<std::int64_t>(static_cast<__int128>(n) * (maker ? cfg_.fees.maker : cfg_.fees.taker) / 1'000'000);
    acct_.cash_micros += side == Side::Buy ? -n : n;
    acct_.cash_micros -= fee;
    acct_.fees_micros += fee;
    acct_.volume_micros += n;
    acct_.position[inst] += side == Side::Buy ? qty : -qty;
    acct_.fills++;
    (maker ? acct_.maker_fills : acct_.taker_fills)++;
    report_(ctx_, ExecReport{'F', side, maker ? 'M' : 'T', 0, static_cast<std::uint32_t>(qty), token, px, fee}, inst);
  }

  // Fills our resting orders at (side, price) with up to `avail` units from the
  // real market, honouring queue_ahead. `fill_px` is the execution price.
  void fill_level(int i, Side side, std::int64_t price, std::int64_t avail, std::int64_t fill_px, Handler& h) {
    MatchingEngine& me = inst_[i]->ours;
    const bool through = avail == INT64_MAX;
    me.for_each_at(side, price, [&](std::uint32_t slot, MeOrder& o) {
      if (avail <= 0) return;
      if (!through) {
        const std::int64_t burn = o.queue_ahead < avail ? o.queue_ahead : avail;
        o.queue_ahead -= burn;
        avail -= burn;
        if (avail <= 0) return;
      }
      const std::int64_t want = through ? o.qty : (avail < o.qty ? avail : o.qty);
      me.fill_resting(slot, static_cast<std::uint32_t>(want), fill_px, h);
      if (!through) avail -= want;
    });
  }

  void after_level_change(int i, Side side, std::int64_t price) {
    Instrument& in = *inst_[i];
    MatchingEngine& me = in.ours;
    // Rule 6: cancels ahead of us.
    const std::int64_t shown = side == Side::Buy ? in.mbids.qty_at(price) : in.masks.qty_at(price);
    me.for_each_at(side, price, [&](std::uint32_t, MeOrder& o) {
      if (o.queue_ahead > shown) o.queue_ahead = shown;
    });
    // Rule 5: the opposite touch crossed one of our resting orders.
    Handler h{this, i, true};
    if (side == Side::Sell) {
      const std::int64_t a = in.masks.best_price();
      for (std::int64_t p = me.bids().best_price(); a != AskLadder::kNone && p != BidLadder::kNone && p >= a;
           p = me.bids().best_price())
        fill_level(i, Side::Buy, p, INT64_MAX, p, h);
    } else {
      const std::int64_t b = in.mbids.best_price();
      for (std::int64_t p = me.asks().best_price(); b != BidLadder::kNone && p != AskLadder::kNone && p <= b;
           p = me.asks().best_price())
        fill_level(i, Side::Sell, p, INT64_MAX, p, h);
    }
  }

  void apply(const OrderMsgView& m) {
    const int i = static_cast<int>(m.instrument());
    if (i < 0 || i >= static_cast<int>(inst_.size())) { reject(m.token(), m.side(), 'I', i); return; }
    if (m.type() != 'O') { reject(m.token(), m.side(), 'T', i); return; }
    if (m.qty() == 0 || m.qty() > cfg_.max_order_qty) { reject(m.token(), m.side(), 'Q', i); return; }
    acct_.orders++;
    Instrument& in = *inst_[i];
    std::int64_t left = m.qty();
    const Side s = m.side();
    const std::int64_t limit = m.price();
    // Rule 2: take displayed liquidity.
    if (s == Side::Buy) {
      for (std::int64_t p = in.masks.best_price(); left && p != AskLadder::kNone && p <= limit; p = in.masks.next_worse(p)) {
        const std::int64_t avail = in.masks.qty_at(p) - in.taken[1].get(p);
        if (avail <= 0) continue;
        const std::int64_t q = avail < left ? avail : left;
        in.taken[1].add(p, q);
        book_fill(i, s, m.token(), p, q, false);
        left -= q;
      }
    } else {
      for (std::int64_t p = in.mbids.best_price(); left && p != BidLadder::kNone && p >= limit; p = in.mbids.next_worse(p)) {
        const std::int64_t avail = in.mbids.qty_at(p) - in.taken[0].get(p);
        if (avail <= 0) continue;
        const std::int64_t q = avail < left ? avail : left;
        in.taken[0].add(p, q);
        book_fill(i, s, m.token(), p, q, false);
        left -= q;
      }
    }
    if (!left) return;
    if (m.tif() == 'I') {
      report_(ctx_, ExecReport{'C', s, ' ', 0, static_cast<std::uint32_t>(left), m.token(), limit, 0}, i);
      return;
    }
    // Rule 3: rest at the back of the displayed queue.
    const std::int64_t ahead = s == Side::Buy ? in.mbids.qty_at(limit) : in.masks.qty_at(limit);
    Handler h{this, i, true};
    in.ours.submit(m.token(), m.account(), s, limit, static_cast<std::uint32_t>(left), Tif::Day, ahead, h);
  }

  PaperConfig cfg_;
  ReportFn report_;
  void* ctx_;
  std::vector<std::unique_ptr<Instrument>> inst_;
  std::vector<Pending> pending_;
  std::size_t head_ = 0, count_ = 0;
  PaperAccount acct_;
};

// Adapter: feeds ITCH messages into the exchange's mirror book and trade logic.
class ItchPaperFeed : public itch::NullHandler {
 public:
  ItchPaperFeed(PaperExchange& x, const std::vector<std::uint16_t>& locates) : x_(x), book_(cfg()) {
    for (auto l : locates) {
      const int b = book_.add_instrument(l);
      if (b < 64) locate_of_book_[b] = l;
    }
  }
  void on_add(const itch::AddOrder& m) { level_after(book_.add(m.ref(), m.locate(), m.side(), m.price(), m.shares()), m.side(), m.price() / 100); }
  void on_executed(const itch::OrderExecuted& m) { exec(m.ref(), m.shares()); }
  void on_executed_price(const itch::OrderExecutedWithPrice& m) { exec(m.ref(), m.shares()); }
  void on_cancel(const itch::OrderCancel& m) { reduce(m.ref(), m.shares()); }
  void on_delete(const itch::OrderDelete& m) { reduce(m.ref(), UINT32_MAX); }
  void on_replace(const itch::OrderReplace& m) {
    std::int64_t px = 0;
    Side s{};
    const auto ev = book_.reduce(m.old_ref(), UINT32_MAX, &px, &s);
    if (ev.book < 0) return;
    sync(ev.book, s, px);
    // Re-add under the new ref (same side, same instrument).
    const std::uint16_t loc = locate_of_book_[ev.book];
    const auto ev2 = book_.add(m.new_ref(), loc, s, m.price(), m.shares());
    if (ev2.book >= 0) sync(ev2.book, s, m.price() / 100);
  }
  MarketBook& book() { return book_; }

 private:
  static BookConfig cfg() {
    BookConfig c;
    c.order_map_log2 = 20;
    c.order_pool = 1u << 19;
    return c;
  }
  void level_after(const BookEvent& ev, Side s, std::int64_t px) {
    if (ev.book >= 0) sync(ev.book, s, px);
  }
  void sync(int b, Side s, std::int64_t px) {
    const auto& ib = book_.book(b);
    const std::int64_t q = s == Side::Buy ? ib.bids.qty_at(px) : ib.asks.qty_at(px);
    x_.on_market_level(b, s, px, q);
  }
  void exec(std::uint64_t ref, std::uint32_t qty) {
    std::int64_t px = 0;
    Side s{};
    const auto ev = book_.reduce(ref, qty, &px, &s);
    if (ev.book < 0) return;
    x_.on_market_trade(ev.book, s, px, qty);
    sync(ev.book, s, px);
  }
  void reduce(std::uint64_t ref, std::uint32_t qty) {
    std::int64_t px = 0;
    Side s{};
    const auto ev = book_.reduce(ref, qty, &px, &s);
    if (ev.book >= 0) sync(ev.book, s, px);
  }

 public:
  std::array<std::uint16_t, 64> locate_of_book_{};

 private:
  PaperExchange& x_;
  MarketBook book_;
};

}  // namespace lle
