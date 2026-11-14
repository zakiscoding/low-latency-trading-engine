#pragma once
// The hot path: ITCH message -> book update -> strategy -> risk -> order -> send.
//
// One instance runs on one pinned thread. It allocates nothing after construction
// (with the default policies), takes no locks, does no logging, and makes no
// system calls except the Sender's send(). Stage boundaries are stamped with
// rdtsc when kTiming is on, so per-stage histograms can be reported.
//
// Policies exist so the before/after table can swap one piece at a time:
//   BookT     MarketBook (flat arrays) | NaiveBook (std::map)
//   BuilderT  PreserializedBuilder     | TextBuilder (std::string formatting)
//   kCopyParse  false: in-place views  | true: allocate + eagerly decode each message
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lle/book.hpp"
#include "lle/common.hpp"
#include "lle/histogram.hpp"
#include "lle/itch.hpp"
#include "lle/naive_book.hpp"
#include "lle/order_codec.hpp"
#include "lle/risk.hpp"
#include "lle/strategy.hpp"
#include "lle/tsc.hpp"

namespace lle {

struct Top {
  std::int64_t bid, bid_qty, ask, ask_qty;
};

inline Top top_of(const MarketBook& mb, int b) noexcept {
  const InstrumentBook& ib = mb.book(b);
  return {ib.bids.best_price(), ib.bids.best_qty(), ib.asks.best_price(), ib.asks.best_qty()};
}
inline Top top_of(const NaiveBook& nb, int b) {
  return {nb.best_bid(b), nb.best_bid_qty(b), nb.best_ask(b), nb.best_ask_qty(b)};
}

// ---- order builders -------------------------------------------------------

struct PreserializedBuilder {
  static constexpr int kMaxBooks = 64;
  std::array<std::array<OrderTemplate, 2>, kMaxBooks> tmpl;

  void init(int books, std::uint64_t account) noexcept {
    for (int b = 0; b < books && b < kMaxBooks; ++b) {
      tmpl[b][0].init('O', Side::Buy, 'I', static_cast<std::uint32_t>(b), account);
      tmpl[b][1].init('O', Side::Sell, 'I', static_cast<std::uint32_t>(b), account);
    }
  }
  LLE_ALWAYS_INLINE const unsigned char* build(int b, Side s, std::uint64_t token, std::uint32_t qty,
                                               std::int64_t px, std::uint64_t tsc, std::int64_t feed_ts,
                                               std::size_t& len) noexcept {
    OrderTemplate& t = tmpl[b][static_cast<int>(s)];
    t.patch(token, qty, px, tsc, feed_ts);
    len = kOrderMsgSize;
    return t.bytes;
  }
};

// Baseline: formats a text order each time (allocates). Produces the same binary
// message at the end so the exchange side doesn't change, but pays for building a
// std::string first the way a FIX-style gateway would.
struct TextBuilder {
  std::string last;
  OrderTemplate out;
  std::uint64_t account = 0;
  void init(int, std::uint64_t acct) { account = acct; }
  const unsigned char* build(int b, Side s, std::uint64_t token, std::uint32_t qty, std::int64_t px,
                             std::uint64_t tsc, std::int64_t feed_ts, std::size_t& len) {
    last = "35=D|11=" + std::to_string(token) + "|55=" + std::to_string(b) + "|54=" + (s == Side::Buy ? "1" : "2") +
           "|38=" + std::to_string(qty) + "|44=" + std::to_string(px) + "|59=3|";
    // Parse it back into the wire format, as a text-to-binary gateway would.
    out.init('O', s, 'I', static_cast<std::uint32_t>(b), account);
    out.patch(std::stoull(last.substr(8, last.find('|', 8) - 8)), qty, px, tsc, feed_ts);
    len = kOrderMsgSize;
    return out.bytes;
  }
};

// ---- per-stage latency ----------------------------------------------------

enum Stage { kParse = 0, kBook, kDecide, kBuild, kSend, kTickToTrade, kBookOnly, kNumStages };
inline constexpr const char* kStageNames[kNumStages] = {"parse", "book", "decide", "build", "send",
                                                        "tick_to_trade", "book_only"};

struct StageHists {
  std::array<Histogram, kNumStages> h;  // in TSC cycles
};

// ---- sinks ------------------------------------------------------------------

struct NullTelemetry {
  LLE_ALWAYS_INLINE void on_order(int, Side, std::int64_t, std::int64_t, std::uint64_t) noexcept {}
  LLE_ALWAYS_INLINE void on_top(int, const Top&) noexcept {}
  LLE_ALWAYS_INLINE void on_trade(int, Side, std::int64_t, std::int64_t) noexcept {}
  LLE_ALWAYS_INLINE void on_t2t(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                std::uint64_t) noexcept {}
};

// Records sent orders in memory (tests, deterministic replay).
struct CaptureSender {
  std::vector<std::array<unsigned char, kOrderMsgSize>> sent;
  bool send(const unsigned char* p, std::size_t n) {
    std::array<unsigned char, kOrderMsgSize> a{};
    std::memcpy(a.data(), p, n < a.size() ? n : a.size());
    sent.push_back(a);
    return true;
  }
};

// Discards orders but keeps the bytes observable so the compiler can't drop the work.
struct SinkSender {
  std::uint64_t n = 0, x = 0;
  LLE_ALWAYS_INLINE bool send(const unsigned char* p, std::size_t len) noexcept {
    n++;
    x ^= load_be<std::uint64_t>(p + ord::kToken) + len;
    return true;
  }
};

// ---- the engine -------------------------------------------------------------

struct EngineCounters {
  std::uint64_t messages = 0, top_changes = 0, decisions = 0, orders = 0, risk_rejects = 0, send_fail = 0;
  std::array<std::uint64_t, 9> reject_by_reason{};
};

template <class BookT, class SenderT, class BuilderT = PreserializedBuilder, class TelemetryT = NullTelemetry,
          bool kTiming = true, bool kCopyParse = false>
class ItchEngine {
 public:
  ItchEngine(BookT& book, SenderT& tx, TelemetryT& tel, const StrategyParams& sp, const RiskLimits& rl,
             const TscClock& clk, std::uint64_t account = 1)
      : book_(book), tx_(tx), tel_(tel), strat_(sp), risk_(rl, clk.ticks_per_ns), clk_(clk) {
    builder_.init(static_cast<int>(book.num_books()), account);
    pos_.fill(0);
    last_order_.fill(0);
  }

  // Called once per received packet/message batch, right after recv returns.
  LLE_ALWAYS_INLINE void begin_packet(std::uint64_t rx_tsc) noexcept { rx_tsc_ = rx_tsc; }

  // Feed one MoldUDP64 packet.
  LLE_ALWAYS_INLINE void on_packet(const unsigned char* p, std::size_t n) noexcept {
    msg_start_ = rx_tsc_;
    if constexpr (kCopyParse) {
      parse_mold_copy(p, n);
    } else {
      itch::parse_mold(p, n, *this, stats_);
    }
  }
  // Feed a single ITCH message (file replay without network).
  LLE_ALWAYS_INLINE void on_message(const unsigned char* p, std::size_t n) noexcept {
    msg_start_ = kTiming ? rdtsc() : 0;
    rx_tsc_ = msg_start_;
    if constexpr (kCopyParse) {
      copy_dispatch(p, n);
    } else {
      itch::dispatch(p, n, *this, stats_);
    }
  }

  // ---- ITCH handler interface ----
  LLE_ALWAYS_INLINE void on_add(const itch::AddOrder& m) noexcept {
    const std::uint64_t ref = m.ref();
    const std::uint16_t loc = m.locate();
    const Side side = m.side();
    const std::uint32_t px = m.price(), qty = m.shares();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(ref ^ loc ^ px ^ qty ^ ts);
    finish(book_.add(ref, loc, side, px, qty), ts);
  }
  LLE_ALWAYS_INLINE void on_executed(const itch::OrderExecuted& m) noexcept {
    const std::uint64_t ref = m.ref();
    const std::uint32_t qty = m.shares();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(ref ^ qty ^ ts);
    std::int64_t px = 0;
    Side s = Side::Buy;
    const BookEvent ev = book_.reduce(ref, qty, &px, &s);
    if (ev.book >= 0) tel_.on_trade(ev.book, s, px, qty);
    finish(ev, ts);
  }
  LLE_ALWAYS_INLINE void on_executed_price(const itch::OrderExecutedWithPrice& m) noexcept {
    const std::uint64_t ref = m.ref();
    const std::uint32_t qty = m.shares();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(ref ^ qty ^ ts);
    std::int64_t px = 0;
    Side s = Side::Buy;
    const BookEvent ev = book_.reduce(ref, qty, &px, &s);
    if (ev.book >= 0) tel_.on_trade(ev.book, s, static_cast<std::int64_t>(m.price() / 100u), qty);
    finish(ev, ts);
  }
  LLE_ALWAYS_INLINE void on_cancel(const itch::OrderCancel& m) noexcept {
    const std::uint64_t ref = m.ref();
    const std::uint32_t qty = m.shares();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(ref ^ qty ^ ts);
    finish(book_.reduce(ref, qty), ts);
  }
  LLE_ALWAYS_INLINE void on_delete(const itch::OrderDelete& m) noexcept {
    const std::uint64_t ref = m.ref();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(ref ^ ts);
    finish(book_.remove(ref), ts);
  }
  LLE_ALWAYS_INLINE void on_replace(const itch::OrderReplace& m) noexcept {
    const std::uint64_t o = m.old_ref(), n = m.new_ref();
    const std::uint32_t qty = m.shares(), px = m.price();
    const std::uint64_t ts = m.timestamp_ns();
    after_parse(o ^ n ^ qty ^ px ^ ts);
    finish(book_.replace(o, n, qty, px), ts);
  }
  void on_trade(const itch::Trade& m) noexcept {
    const int b = book_.book_for(m.locate());
    if (b >= 0) tel_.on_trade(b, m.side(), static_cast<std::int64_t>(m.price() / 100u), m.shares());
    next_message();
  }
  void on_system(const itch::SystemEvent&) noexcept { next_message(); }
  void on_directory(const itch::StockDirectory&) noexcept { next_message(); }
  void on_other(const itch::MsgView&) noexcept { next_message(); }

  // Fill/position feedback from the exchange (called off the critical path).
  void on_exec_report(const ExecReport& r, int book_idx) noexcept {
    if (r.type == 'F' && book_idx >= 0 && book_idx < static_cast<int>(pos_.size())) {
      pos_[book_idx] += r.side == Side::Buy ? static_cast<std::int64_t>(r.qty) : -static_cast<std::int64_t>(r.qty);
    }
  }

  StageHists& hists() noexcept { return hists_; }
  const EngineCounters& counters() const noexcept { return c_; }
  const itch::ParseStats& parse_stats() const noexcept { return stats_; }
  RiskGate& risk() noexcept { return risk_; }
  ImbalanceStrategy& strategy() noexcept { return strat_; }
  std::int64_t position(int b) const noexcept { return pos_[b]; }
  void set_trading_enabled(bool on) noexcept { trading_ = on; }
  bool trading_enabled() const noexcept { return trading_; }

 private:
  LLE_ALWAYS_INLINE void after_parse(std::uint64_t fields) noexcept {
    c_.messages++;
    if constexpr (kTiming) {
      keep(fields);
      t_parsed_ = rdtsc();
    }
  }
  LLE_ALWAYS_INLINE void next_message() noexcept {
    if constexpr (kTiming) msg_start_ = rdtsc_unordered();
  }

  LLE_ALWAYS_INLINE void finish(const BookEvent& ev, std::uint64_t feed_ts) noexcept {
    std::uint64_t t_book = 0;
    if constexpr (kTiming) {
      t_book = rdtsc();
      hists_.h[kParse].record(t_parsed_ - msg_start_);
      hists_.h[kBook].record(t_book - t_parsed_);
    }
    if (ev.book < 0 || !ev.top_changed) {
      if constexpr (kTiming) {
        hists_.h[kBookOnly].record(t_book - msg_start_);
        msg_start_ = t_book;
      }
      return;
    }
    c_.top_changes++;
    const int b = ev.book;
    const Top t = top_of(book_, b);
    const std::uint64_t now = kTiming ? t_book : rdtsc_unordered();
    const Decision d = trading_ ? strat_.decide(t.bid, t.bid_qty, t.ask, t.ask_qty, pos_[b], now, last_order_[b])
                                : Decision{};
    std::uint64_t t_dec = 0;
    if constexpr (kTiming) {
      t_dec = rdtsc();
      hists_.h[kDecide].record(t_dec - t_book);
    }
    if (d.act) {
      c_.decisions++;
      const std::int64_t touch = d.side == Side::Buy ? t.ask : t.bid;
      const RiskReject rr = risk_.check(d.side, d.qty, d.price, pos_[b], touch, now);
      if (LLE_UNLIKELY(rr != RiskReject::None)) {
        c_.risk_rejects++;
        c_.reject_by_reason[static_cast<int>(rr)]++;
      } else {
        std::size_t len = 0;
        const std::uint64_t token = ++token_;
        const unsigned char* msg = builder_.build(b, d.side, token, static_cast<std::uint32_t>(d.qty), d.price,
                                                  kTiming ? t_dec : 0, static_cast<std::int64_t>(feed_ts), len);
        std::uint64_t t_built = 0;
        if constexpr (kTiming) t_built = rdtsc();
        const bool ok = tx_.send(msg, len);
        if (LLE_UNLIKELY(!ok)) c_.send_fail++;
        c_.orders++;
        if constexpr (kTiming) {
          const std::uint64_t t_sent = rdtsc();
          hists_.h[kBuild].record(t_built - t_dec);
          hists_.h[kSend].record(t_sent - t_built);
          hists_.h[kTickToTrade].record(t_sent - rx_tsc_);
          tel_.on_t2t(t_parsed_ - msg_start_, t_book - t_parsed_, t_dec - t_book, t_built - t_dec,
                      t_sent - t_built, t_sent - rx_tsc_);
        }
        tel_.on_order(b, d.side, d.price, d.qty, token);
        last_sent_tsc_ = kTiming ? rdtsc_unordered() : 0;
      }
    }
    tel_.on_top(b, t);
    if constexpr (kTiming) msg_start_ = rdtsc_unordered();
  }

  // ---- naive parse baseline: allocate and eagerly decode every message ----
  struct Decoded {
    char type;
    std::uint16_t locate;
    std::uint64_t ts, ref, new_ref;
    char side;
    std::uint32_t shares, price;
    std::string stock;
    std::vector<unsigned char> raw;
  };
  void copy_dispatch(const unsigned char* p, std::size_t n) {
    if (n == 0 || itch::spec_len(static_cast<char>(p[0])) == 0 || n < itch::spec_len(static_cast<char>(p[0]))) {
      stats_.malformed++;
      return;
    }
    auto d = std::make_unique<Decoded>();
    d->raw.assign(p, p + n);
    const unsigned char* q = d->raw.data();
    d->type = static_cast<char>(q[0]);
    d->locate = load_be<std::uint16_t>(q + 1);
    d->ts = load_be48(q + 5);
    stats_.messages++;
    // Re-dispatch through the in-place views over the copy.
    itch::ParseStats dummy;
    itch::dispatch(q, n, *this, dummy);
  }
  void parse_mold_copy(const unsigned char* p, std::size_t n) {
    if (n < itch::kMoldHeader) return;
    const std::uint16_t count = load_be<std::uint16_t>(p + 18);
    std::size_t off = itch::kMoldHeader;
    for (std::uint16_t i = 0; i < count && off + 2 <= n; ++i) {
      const std::size_t len = load_be<std::uint16_t>(p + off);
      if (off + 2 + len > n) break;
      copy_dispatch(p + off + 2, len);
      off += 2 + len;
    }
  }

  BookT& book_;
  SenderT& tx_;
  TelemetryT& tel_;
  BuilderT builder_;
  ImbalanceStrategy strat_;
  RiskGate risk_;
  const TscClock& clk_;
  itch::ParseStats stats_;
  EngineCounters c_;
  StageHists hists_;
  std::array<std::int64_t, 64> pos_;
  std::array<std::uint64_t, 64> last_order_;
  std::uint64_t token_ = 0;
  std::uint64_t rx_tsc_ = 0, msg_start_ = 0, t_parsed_ = 0, last_sent_tsc_ = 0;
  bool trading_ = true;
};

}  // namespace lle
