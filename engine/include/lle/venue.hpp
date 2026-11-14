#pragma once
// Live crypto venues behind one interface: connection, subscriptions and parsing.
//   coinbase  public Advanced Trade feed. Its market data terms forbid showing the
//             data or anything derived from it to others: private runs only.
//   bitstamp  public WebSocket v2. Bitstamp allows redistribution of its exchange
//             data: this is the venue behind the public web UI.
#include <cctype>
#include <stdexcept>
#include <string>

#include "lle/bitstamp.hpp"
#include "lle/coinbase.hpp"
#include "lle/ws.hpp"

namespace lle {

enum class Venue { Coinbase, Bitstamp };

inline Venue parse_venue(const std::string& s) {
  if (s == "coinbase") return Venue::Coinbase;
  if (s == "bitstamp") return Venue::Bitstamp;
  throw std::runtime_error("unknown venue: " + s + " (coinbase|bitstamp)");
}
inline const char* venue_name(Venue v) { return v == Venue::Coinbase ? "coinbase" : "bitstamp"; }
// True if the venue's terms allow showing its data publicly.
inline bool venue_public_display_allowed(Venue v) { return v == Venue::Bitstamp; }

class VenueFeed {
 public:
  // product is written "BTC-USD"; Bitstamp's pair name is derived ("btcusd").
  VenueFeed(Venue v, std::string product) : v_(v), product_(std::move(product)) {
    for (char c : product_)
      if (c != '-') pair_ += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  void connect(ws::Client& c) {
    if (v_ == Venue::Coinbase) {
      c.connect("advanced-trade-ws.coinbase.com", "/");
      for (const char* ch : {"level2", "market_trades", "heartbeats"})
        c.send_text("{\"type\":\"subscribe\",\"product_ids\":[\"" + product_ + "\"],\"channel\":\"" + ch + "\"}");
    } else {
      c.connect("ws.bitstamp.net", "/");
      for (const std::string ch : {"diff_order_book_", "order_book_", "live_trades_"})
        c.send_text("{\"event\":\"bts:subscribe\",\"data\":{\"channel\":\"" + ch + pair_ + "\"}}");
    }
  }

  template <class H>
  LLE_ALWAYS_INLINE bool parse(const unsigned char* m, std::size_t n, H& h, coinbase::Stats& st) noexcept {
    const char* p = reinterpret_cast<const char*>(m);
    return v_ == Venue::Coinbase ? coinbase::parse(p, n, h, st) : bitstamp::parse(p, n, h, st, bs_);
  }

  Venue venue() const { return v_; }
  const std::string& product() const { return product_; }

 private:
  Venue v_;
  std::string product_, pair_;
  bitstamp::State bs_;
};

}  // namespace lle
