#pragma once
// Paper-trading guard. Every binary calls enforce_paper_only() before doing
// anything else; it aborts the process if it sees a live trading key or endpoint.
//
// Rules:
//  - Market-data hosts are allowed (they can't place orders).
//  - The only order-entry host that is allowed is Alpaca's paper API, which is
//    hard-coded below and can't be overridden by config.
//  - Any other *.alpaca.markets API host is a live endpoint: refuse.
//  - Alpaca live keys start with "AK" (paper keys start with "PK"): refuse.
//  - Any Coinbase API key/secret in the environment: refuse (this project never
//    needs one; the market data it reads is public).
//
// The live hostnames are deliberately not spelled out in this file, so the CI scan
// for live trading URLs (tools/check_no_live_urls.py) stays meaningful.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace lle::safety {

inline constexpr std::string_view kAlpacaPaperHost = "paper-api.alpaca.markets";
inline constexpr std::string_view kAlpacaPaperBase = "https://paper-api.alpaca.markets";

inline constexpr std::string_view kAllowedDataHosts[] = {
    "advanced-trade-ws.coinbase.com",  // public market data
    "ws-feed.exchange.coinbase.com",   // public market data
    "stream.data.alpaca.markets",      // market data (needs paper keys, read only)
    "data.alpaca.markets",
    "127.0.0.1",
    "localhost",
};

inline bool ends_with(std::string_view s, std::string_view suf) {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// Extracts the host from "scheme://host[:port]/path" (or returns the input).
inline std::string_view host_of(std::string_view url) {
  if (auto p = url.find("://"); p != std::string_view::npos) url = url.substr(p + 3);
  if (auto p = url.find_first_of("/:?#"); p != std::string_view::npos) url = url.substr(0, p);
  return url;
}

enum class Verdict { Allowed, Refused };

// Loopback and RFC 1918 IPv4 literals: the local paper exchange and telemetry.
inline bool is_private_ipv4(std::string_view h) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  const std::string hs(h);
  if (std::sscanf(hs.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

inline Verdict check_endpoint(std::string_view url, std::string* why = nullptr) {
  const std::string_view h = host_of(url);
  if (h == kAlpacaPaperHost) return Verdict::Allowed;
  for (auto a : kAllowedDataHosts)
    if (h == a) return Verdict::Allowed;
  if (is_private_ipv4(h)) return Verdict::Allowed;
  if (ends_with(h, "alpaca.markets")) {
    if (why) *why = "non-paper Alpaca host";
    return Verdict::Refused;
  }
  if (ends_with(h, "coinbase.com")) {
    if (why) *why = "Coinbase host that is not a public market-data feed";
    return Verdict::Refused;
  }
  if (why) *why = "host not on the allowlist";
  return Verdict::Refused;
}

inline bool is_live_alpaca_key(std::string_view key) { return key.size() >= 2 && key.substr(0, 2) == "AK"; }

// Returns an empty string if OK, otherwise the reason to refuse.
inline std::string scan_environment(char** envp) {
  for (char** e = envp; e && *e; ++e) {
    std::string_view kv(*e);
    const auto eq = kv.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (v.empty()) continue;
    if ((k == "APCA_API_KEY_ID" || k == "ALPACA_API_KEY" || k == "ALPACA_KEY_ID") && is_live_alpaca_key(v))
      return std::string(k) + " looks like a live Alpaca key (starts with AK)";
    if (k == "APCA_API_BASE_URL" || k == "ALPACA_BASE_URL" || k == "ALPACA_TRADING_URL") {
      if (host_of(v) != kAlpacaPaperHost) return std::string(k) + " is not the Alpaca paper endpoint";
    }
    if (k.starts_with("COINBASE_API") || k.starts_with("CB_ACCESS") || k == "COINBASE_KEY" || k == "COINBASE_SECRET")
      return std::string(k) + " is set: this project never uses Coinbase credentials";
  }
  return {};
}

// Aborts if anything looks live. Call first thing in main().
inline void enforce_paper_only(char** envp) {
  const std::string why = scan_environment(envp);
  if (!why.empty()) {
    std::fprintf(stderr, "REFUSING TO START (paper trading only): %s\n", why.c_str());
    std::exit(78);  // EX_CONFIG
  }
}

inline void require_allowed(std::string_view url) {
  std::string why;
  if (check_endpoint(url, &why) != Verdict::Allowed) {
    std::fprintf(stderr, "REFUSING endpoint %.*s: %s\n", static_cast<int>(url.size()), url.data(), why.c_str());
    std::exit(78);
  }
}

}  // namespace lle::safety
