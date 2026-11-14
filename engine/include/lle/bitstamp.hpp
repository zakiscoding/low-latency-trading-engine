#pragma once
// In-place parser for Bitstamp's public WebSocket v2 (wss://ws.bitstamp.net).
// Bitstamp's API page allows redistribution of its exchange data, which is why
// the public web UI uses this venue.
//
// Channels:
//   order_book_<pair>       top-100 snapshot every ~100 ms  -> replaces the book each time
//   diff_order_book_<pair>  changed levels, absolute qty, 0 = remove
//   live_trades_<pair>      trades; "type": 0 = buy (aggressor), 1 = sell
// Produces the same handler callbacks as coinbase::parse, so the engine, paper
// exchange and recorder take either venue.
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lle/coinbase.hpp"

namespace lle::bitstamp {

using coinbase::L2Update;
using coinbase::Stats;
using coinbase::TradePrint;
using coinbase::find;
using coinbase::parse_fixed;

struct State {
  bool seeded = false;
  std::int64_t snapshot_us = 0;  // diffs at or before this are already in the snapshot
};

LLE_ALWAYS_INLINE bool parse_uint(const char* p, const char* end, std::int64_t& v) noexcept {
  v = 0;
  bool any = false;
  while (p < end && static_cast<unsigned>(*p - '0') <= 9) {
    v = v * 10 + (*p - '0');
    ++p;
    any = true;
  }
  return any;
}

// Parses [["p","q"],["p","q"],...] starting right after "bids":[ . Returns pointer
// after the closing ']' of the outer array, or nullptr.
template <class H>
const char* levels(const char* p, const char* end, bool bid, H& h, Stats& st) noexcept {
  for (;;) {
    while (p < end && (*p == ' ' || *p == ',')) ++p;
    if (p >= end) return nullptr;
    if (*p == ']') return p + 1;
    if (end - p < 3 || p[0] != '[' || p[1] != '"') return nullptr;
    L2Update u{bid, 0, 0};
    const char* nx;
    if (!parse_fixed(p + 2, end, 2, u.price, nx)) return nullptr;
    if (end - nx < 2 || nx[0] != ',' || nx[1] != '"') return nullptr;
    if (!parse_fixed(nx + 2, end, 8, u.qty, nx)) return nullptr;
    if (nx >= end || *nx != ']') return nullptr;
    h.on_l2(u);
    st.updates++;
    p = nx + 1;
  }
}

template <class H>
bool parse(const char* p, std::size_t n, H& h, Stats& st, State& s) noexcept {
  const char* end = p + n;
  st.messages++;
  const char* ch = find(p, end, "\"channel\":\"");
  if (!ch) { st.malformed++; return false; }
  const char* che = static_cast<const char*>(std::memchr(ch, '"', static_cast<std::size_t>(end - ch)));
  if (!che) { st.malformed++; return false; }
  const std::string_view chan(ch, static_cast<std::size_t>(che - ch));
  const char* ev = find(p, end, "\"event\":\"");
  if (!ev) { st.malformed++; return false; }
  if (std::string_view(ev, std::min<std::size_t>(4, static_cast<std::size_t>(end - ev))) == "bts:") return true;  // control
  const bool snap = chan.rfind("order_book_", 0) == 0;
  const bool diff = chan.rfind("diff_order_book_", 0) == 0;
  const bool trades = chan.rfind("live_trades_", 0) == 0;
  if (snap || diff) {
    const char* mt = find(p, end, "\"microtimestamp\":\"");
    std::int64_t us = 0;
    if (!mt || !parse_uint(mt, end, us)) { st.malformed++; return false; }
    // Every top-100 snapshot (~10/s) replaces the book; only diffs newer than the
    // latest snapshot are applied. Diffs and snapshots can arrive out of order, so
    // seeding once and trusting diffs afterwards leaves stale levels (seen live: a
    // crossed book). This heals within one snapshot interval.
    if (snap && us <= s.snapshot_us) return true;
    if (diff && (!s.seeded || us <= s.snapshot_us)) return true;
    const char* b = find(p, end, "\"bids\":[");
    if (!b) { st.malformed++; return false; }
    const std::string_view product = snap ? chan.substr(11) : chan.substr(16);
    h.on_l2_begin(product, snap);
    const char* after = levels(b, end, true, h, st);
    const char* a = after ? find(after, end, "\"asks\":[") : nullptr;
    if (!a || !levels(a, end, false, h, st)) { st.malformed++; h.on_l2_end(); return false; }
    h.on_l2_end();
    if (snap) {
      s.seeded = true;
      s.snapshot_us = us;
      st.snapshots++;
    }
    return true;
  }
  if (trades) {
    const char* am = find(p, end, "\"amount_str\":\"");
    const char* pr = find(p, end, "\"price_str\":\"");
    const char* ty = find(p, end, "\"type\":");
    if (!am || !pr || !ty) { st.malformed++; return false; }
    TradePrint t{};
    const char* nx;
    if (!parse_fixed(pr, end, 2, t.price, nx) || !parse_fixed(am, end, 8, t.qty, nx)) { st.malformed++; return false; }
    t.buy = ty < end && *ty == '0';
    st.trades++;
    h.on_trade(chan.substr(12), t);
    return true;
  }
  st.malformed++;
  return false;
}

}  // namespace lle::bitstamp
