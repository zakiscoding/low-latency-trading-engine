#pragma once
// In-place parser for Coinbase Advanced Trade public WebSocket messages
// (channels level2 -> "l2_data", market_trades, heartbeats).
//
// This is not a general JSON parser. It relies on the documented, stable field
// order inside each update object and scans the decrypted buffer once, left to
// right, with memchr/memcmp. Numbers are parsed straight from their quoted
// decimal strings into fixed point (price in ticks, quantity in 1e-8 units):
// no strings, no doubles, no allocation. Anything unexpected makes the message
// count as malformed; it never reads outside [p, p+n) (fuzzed under ASan).
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lle/common.hpp"

namespace lle::coinbase {

enum class Channel : std::uint8_t { Unknown, L2, Trades, Heartbeat, Subscriptions };

struct L2Update {
  bool bid;
  std::int64_t price;  // ticks
  std::int64_t qty;    // 1e-8 units
};
struct TradePrint {
  bool buy;  // "side" field as sent ("BUY"/"SELL")
  std::int64_t price;
  std::int64_t qty;
};

struct Stats {
  std::uint64_t messages = 0, malformed = 0, updates = 0, trades = 0, snapshots = 0, seq_gaps = 0;
};

// Parses "123.45" -> 12345 with `dec` implied decimals. Extra decimals are
// truncated; missing ones are zero-padded. Stops at the closing quote.
// Returns false on anything other than digits and at most one '.'.
LLE_ALWAYS_INLINE bool parse_fixed(const char* s, const char* end, int dec, std::int64_t& out, const char*& next) noexcept {
  std::int64_t v = 0;
  int frac = -1;
  const char* p = s;
  for (; p < end && *p != '"'; ++p) {
    const char c = *p;
    if (c == '.') {
      if (frac >= 0) return false;
      frac = 0;
      continue;
    }
    if (static_cast<unsigned>(c - '0') > 9) return false;
    if (frac >= 0) {
      if (frac >= dec) continue;  // truncate
      frac++;
    }
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (c - '0');
  }
  if (p >= end || p == s) return false;
  for (int k = frac < 0 ? 0 : frac; k < dec; ++k) {
    if (v > INT64_MAX / 10) return false;
    v *= 10;
  }
  out = v;
  next = p + 1;
  return true;
}

// Finds `key` (including its quotes and colon, e.g. "\"price_level\":\"") at or after p.
LLE_ALWAYS_INLINE const char* find(const char* p, const char* end, std::string_view key) noexcept {
  if (p >= end || static_cast<std::size_t>(end - p) < key.size()) return nullptr;
  const char* last = end - key.size();
  while (p <= last) {
    const void* q = std::memchr(p, key[0], static_cast<std::size_t>(last - p + 1));
    if (!q) return nullptr;
    p = static_cast<const char*>(q);
    if (std::memcmp(p, key.data(), key.size()) == 0) return p + key.size();
    ++p;
  }
  return nullptr;
}

struct Config {
  int price_decimals = 2;  // BTC-USD tick is 0.01
  int qty_decimals = 8;
};

// Handler: on_l2_begin(product, snapshot), on_l2(update), on_l2_end(),
//          on_trade(product, trade), on_heartbeat(), on_sequence(seq)
template <class H>
bool parse(const char* p, std::size_t n, H& h, Stats& st, const Config& cfg = {}) noexcept {
  const char* end = p + n;
  st.messages++;
  const char* c = find(p, end, "\"channel\":\"");
  if (!c) { st.malformed++; return false; }
  const char* ce = static_cast<const char*>(std::memchr(c, '"', static_cast<std::size_t>(end - c)));
  if (!ce) { st.malformed++; return false; }
  const std::string_view ch(c, static_cast<std::size_t>(ce - c));
  if (const char* s = find(ce, end, "\"sequence_num\":")) {
    std::int64_t seq = 0;
    bool any = false;
    while (s < end && static_cast<unsigned>(*s - '0') <= 9) { seq = seq * 10 + (*s - '0'); ++s; any = true; }
    if (any) h.on_sequence(seq);
  }
  if (ch == "l2_data") {
    // One or more events: {"type":"snapshot|update","product_id":"X","updates":[...]}
    const char* q = ce;
    bool any_event = false;
    while ((q = find(q, end, "\"type\":\""))) {
      const bool snap = (end - q) >= 8 && std::memcmp(q, "snapshot", 8) == 0;
      const char* pid = find(q, end, "\"product_id\":\"");
      if (!pid) { st.malformed++; return false; }
      const char* pe = static_cast<const char*>(std::memchr(pid, '"', static_cast<std::size_t>(end - pid)));
      if (!pe) { st.malformed++; return false; }
      const char* u = find(pe, end, "\"updates\":[");
      if (!u) { st.malformed++; return false; }
      if (snap) st.snapshots++;
      h.on_l2_begin(std::string_view(pid, static_cast<std::size_t>(pe - pid)), snap);
      any_event = true;
      // Each update: {"side":"bid|offer",...,"price_level":"..","new_quantity":".."}
      for (;;) {
        // Stop at the end of this updates array.
        while (u < end && (*u == ' ' || *u == ',')) ++u;
        if (u >= end) { st.malformed++; return false; }
        if (*u == ']') { ++u; break; }
        const char* sd = find(u, end, "\"side\":\"");
        if (!sd || sd >= end) { st.malformed++; return false; }
        const bool bid = *sd == 'b';
        const char* pl = find(sd, end, "\"price_level\":\"");
        if (!pl) { st.malformed++; return false; }
        L2Update up{bid, 0, 0};
        const char* nx;
        if (!parse_fixed(pl, end, cfg.price_decimals, up.price, nx)) { st.malformed++; return false; }
        const char* nq = find(nx, end, "\"new_quantity\":\"");
        if (!nq) { st.malformed++; return false; }
        if (!parse_fixed(nq, end, cfg.qty_decimals, up.qty, nx)) { st.malformed++; return false; }
        st.updates++;
        h.on_l2(up);
        const char* close = static_cast<const char*>(std::memchr(nx, '}', static_cast<std::size_t>(end - nx)));
        if (!close) { st.malformed++; return false; }
        u = close + 1;
      }
      h.on_l2_end();
      q = u;
    }
    if (!any_event) { st.malformed++; return false; }
    return true;
  }
  if (ch == "market_trades") {
    const char* q = ce;
    while ((q = find(q, end, "\"product_id\":\""))) {
      const char* pe = static_cast<const char*>(std::memchr(q, '"', static_cast<std::size_t>(end - q)));
      if (!pe) { st.malformed++; return false; }
      const char* pr = find(pe, end, "\"price\":\"");
      if (!pr) { st.malformed++; return false; }
      TradePrint t{};
      const char* nx;
      if (!parse_fixed(pr, end, cfg.price_decimals, t.price, nx)) { st.malformed++; return false; }
      const char* sz = find(nx, end, "\"size\":\"");
      if (!sz || !parse_fixed(sz, end, cfg.qty_decimals, t.qty, nx)) { st.malformed++; return false; }
      const char* sd = find(nx, end, "\"side\":\"");
      if (!sd || sd >= end) { st.malformed++; return false; }
      t.buy = *sd == 'B';
      st.trades++;
      h.on_trade(std::string_view(q, static_cast<std::size_t>(pe - q)), t);
      q = sd;
    }
    return true;
  }
  if (ch == "heartbeats") { h.on_heartbeat(); return true; }
  if (ch == "subscriptions") return true;
  st.malformed++;
  return false;
}

struct NullHandler {
  void on_l2_begin(std::string_view, bool) {}
  void on_l2(const L2Update&) {}
  void on_l2_end() {}
  void on_trade(std::string_view, const TradePrint&) {}
  void on_heartbeat() {}
  void on_sequence(std::int64_t) {}
};

}  // namespace lle::coinbase
