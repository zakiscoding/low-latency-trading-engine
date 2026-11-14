// Live engine on Coinbase's public Advanced Trade WebSocket (market data only; no
// account, no keys). Orders go over UDP to the local paper exchange, never to
// Coinbase.
//
//   coinbase_engine --product BTC-USD --exchange 127.0.0.1:31012 [--cpu 2]
//                   [--order-qty 10000] (1e-8 BTC units) [--json out.json] [--duration-s 60]
//
// Stages (rdtsc): decode = socket reads + TLS decrypt, once per read; parse_apply = JSON scan + book updates (interleaved, so
// one stage); decide; build; send. kernel_rx_to_send uses the TCP socket's
// SO_TIMESTAMPING RX stamp of the last segment of the message.
#include <cstdio>
#include <fstream>
#include <string>

#include "lle/args.hpp"
#include "lle/coinbase.hpp"
#include "lle/ladder.hpp"
#include "lle/net.hpp"
#include "lle/order_codec.hpp"
#include "lle/report.hpp"
#include "lle/risk.hpp"
#include "lle/safety.hpp"
#include "lle/shm.hpp"
#include "lle/strategy.hpp"
#include "lle/telemetry_sink.hpp"
#include "lle/ws.hpp"

using namespace lle;

namespace {

enum CbStage { cDecode, cParseApply, cDecide, cBuild, cSend, cInproc, cNum };
const char* kCbNames[cNum] = {"decode", "parse_apply", "decide", "build", "send", "inproc_tick_to_trade"};

struct Engine {
  BidLadder bids{18};  // 262k one-cent levels = $2,621 window, recentred on demand
  AskLadder asks{18};
  coinbase::Stats st;
  ImbalanceStrategy strat;
  RiskGate risk;
  OrderTemplate tmpl[2];
  net::Sender& tx;
  ShmTelemetry& tel;
  std::array<Histogram, cNum> h;
  std::int64_t position = 0;
  std::uint64_t token = 0, orders = 0, rejects = 0, last_order = 0, gaps = 0;
  std::int64_t last_seq = -1;
  std::uint64_t t_msg = 0;  // stamp at message start
  bool trading = true;
  bool sent_this_msg = false;
  std::int64_t prev_bid = 0, prev_ask = 0, prev_bq = 0, prev_aq = 0;

  Engine(net::Sender& t, ShmTelemetry& te, const StrategyParams& sp, const RiskLimits& rl, double tpn)
      : strat(sp), risk(rl, tpn), tx(t), tel(te) {
    tmpl[0].init('O', Side::Buy, 'I', 0, 1);
    tmpl[1].init('O', Side::Sell, 'I', 0, 1);
  }

  // coinbase::parse handler
  void on_sequence(std::int64_t s) {
    if (last_seq >= 0 && s != last_seq + 1) gaps++;
    last_seq = s;
  }
  void on_l2_begin(std::string_view, bool snapshot) {
    if (snapshot) { bids.clear(); asks.clear(); }
  }
  LLE_ALWAYS_INLINE void on_l2(const coinbase::L2Update& u) {
    if (u.bid) bids.set(u.price, u.qty); else asks.set(u.price, u.qty);
  }
  void on_l2_end() {}
  void on_trade(std::string_view, const coinbase::TradePrint& t) {
    // Resting side inferred from the book: at/above the ask means a buyer lifted it.
    const Side resting = (asks.best_price() != AskLadder::kNone && t.price >= asks.best_price()) ? Side::Sell
                         : (bids.best_price() != BidLadder::kNone && t.price <= bids.best_price()) ? Side::Buy
                         : (t.buy ? Side::Sell : Side::Buy);
    tel.on_trade(0, resting, t.price, t.qty);
  }
  void on_heartbeat() {}

  // After a full message has been applied.
  LLE_ALWAYS_INLINE void after_message(std::uint64_t t_applied, std::int64_t kernel_rx) {
    const std::int64_t b = bids.best_price(), a = asks.best_price();
    const std::int64_t bq = bids.best_qty(), aq = asks.best_qty();
    if (b == prev_bid && a == prev_ask && bq == prev_bq && aq == prev_aq) return;
    prev_bid = b; prev_ask = a; prev_bq = bq; prev_aq = aq;
    const Decision d = trading ? strat.decide(b, bq, a, aq, position, t_applied, last_order) : Decision{};
    const std::uint64_t t_dec = rdtsc();
    h[cDecide].record(t_dec - t_applied);
    if (d.act) {
      const RiskReject rr = risk.check(d.side, d.qty, d.price, position, d.side == Side::Buy ? a : b, t_dec);
      if (rr != RiskReject::None) {
        rejects++;
      } else {
        OrderTemplate& t = tmpl[static_cast<int>(d.side)];
        t.patch(++token, static_cast<std::uint32_t>(d.qty), d.price, t_dec, kernel_rx);
        const std::uint64_t t_built = rdtsc();
        tx.send(t.bytes, kOrderMsgSize);
        const std::uint64_t t_sent = rdtsc();
        h[cBuild].record(t_built - t_dec);
        h[cSend].record(t_sent - t_built);
        h[cInproc].record(t_sent - t_msg);
        orders++;
        sent_this_msg = true;
        tel.on_order(0, d.side, d.price, d.qty, token);
      }
    }
    tel.on_top(0, Top{b, bq, a, aq});
  }
};

}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  const std::string product = a.str("product", "BTC-USD");
  const std::string exch = a.str("exchange", "127.0.0.1:31012");
  safety::require_allowed("udp://" + exch);
  if (a.num("cpu", -1) >= 0) pin_to_cpu(static_cast<int>(a.num("cpu")));
  lock_memory();
  const TscClock clk = TscClock::calibrate(200);

  net::Sender tx(net::parse_hostport(exch));
  net::set_nonblocking(tx.fd());
  shm::TelemetryRing ring(a.str("shm", "/dev/shm/lle-telemetry-cb"), 16);
  ShmTelemetry tel(ring, clk);
  tel.set_top_rate_hz(50);

  StrategyParams sp;
  sp.order_qty = a.num("order-qty", 10'000);           // 0.0001 BTC
  sp.position_limit = a.num("position-limit", 200'000);  // 0.002 BTC
  sp.cooldown_cycles = static_cast<std::uint64_t>(clk.ticks_per_ns * 1e6 * static_cast<double>(a.num("cooldown-ms", 250)));
  RiskLimits rl;
  rl.max_order_qty = a.num("max-order-qty", 100'000);    // 0.001 BTC
  rl.max_position = a.num("max-position", 500'000);      // 0.005 BTC
  rl.max_notional_ticks = 1ll << 62;
  rl.max_orders_per_sec = static_cast<std::uint32_t>(a.num("max-orders-per-sec", 5));
  rl.price_band_ticks = 10'000;  // $100
  Engine eng(tx, tel, sp, rl, clk.ticks_per_ns);

  ws::Client c;
  c.connect("advanced-trade-ws.coinbase.com", "/");
  for (const char* ch : {"level2", "market_trades", "heartbeats"})
    c.send_text("{\"type\":\"subscribe\",\"product_ids\":[\"" + product + "\"],\"channel\":\"" + ch + "\"}");
  std::fprintf(stderr, "coinbase_engine: %s connected, rx_timestamps=%d, orders -> %s (PAPER ONLY)\n",
               product.c_str(), c.rx_timestamps(), exch.c_str());

  Histogram h_k2s;
  const std::uint64_t one_sec = static_cast<std::uint64_t>(clk.ticks_per_ns * 1e9);
  const std::uint64_t start = rdtsc();
  const std::uint64_t dur = static_cast<std::uint64_t>(a.num("duration-s", 0)) * one_sec;
  std::uint64_t last_print = start, last_snap = start;
  unsigned char exec_buf[128];
  std::uint64_t fills = 0;
  while (!g_stop) {
    // Each message's clock starts when the previous one in the same read finished
    // (or when TLS decrypt finished, for the first), so batching doesn't inflate it.
    const std::uint64_t t_poll = rdtsc_unordered();
    std::uint64_t t_prev_end = 0;
    const bool ok = c.poll([&](const unsigned char* m, std::size_t n, std::int64_t krx) {
      const std::uint64_t t0 = rdtsc();
      if (!t_prev_end) {
        eng.h[cDecode].record(c.decrypted_tsc() - t_poll);  // recvmsg loop + TLS decrypt, once per read
        t_prev_end = c.decrypted_tsc();
      }
      eng.t_msg = t_prev_end;
      eng.sent_this_msg = false;
      coinbase::parse(reinterpret_cast<const char*>(m), n, eng, eng.st);
      const std::uint64_t t1 = rdtsc();
      eng.h[cParseApply].record(t1 - t0);
      eng.after_message(t1, krx);
      t_prev_end = rdtsc_unordered();
      if (eng.sent_this_msg && krx) {
        const std::int64_t d = clock_ns(CLOCK_REALTIME) - krx;
        if (d > 0) h_k2s.record(static_cast<std::uint64_t>(d));
        tel.flush_latency(d);
      }
    });
    if (!ok) {
      std::fprintf(stderr, "coinbase_engine: connection closed\n");
      break;
    }
    // Idle-side work.
    for (;;) {
      const ssize_t n = ::recv(tx.fd(), exec_buf, sizeof(exec_buf), MSG_DONTWAIT);
      if (n < static_cast<ssize_t>(kExecMsgSize)) break;
      const ExecReport r = ExecReport::decode(exec_buf);
      if (r.type == 'F') {
        eng.position += r.side == Side::Buy ? r.qty : -static_cast<std::int64_t>(r.qty);
        fills++;
        tel.on_fill(0, r);
      }
    }
    const std::uint64_t now = rdtsc_unordered();
    if (now - last_snap > one_sec / 20) {
      ring.push(shm::kSnapBegin, 0, 0, 0, 0, 0, 0, 0, 0, 0);
      std::uint32_t k = 0;
      eng.bids.for_each_level(20, [&](std::int64_t p, std::int64_t q, std::uint32_t) { ring.push(shm::kLevel, 0, 0, k++, p, q, 0, 0, 0, 0); });
      k = 0;
      eng.asks.for_each_level(20, [&](std::int64_t p, std::int64_t q, std::uint32_t) { ring.push(shm::kLevel, 0, 1, k++, p, q, 0, 0, 0, 0); });
      ring.push(shm::kSnapEnd, 0, 0, 0, 0, 0, 0, 0, 0, 0);
      last_snap = now;
    }
    if (now - last_print > 10 * one_sec) {
      std::fprintf(stderr, "msgs=%llu updates=%llu bid=%.2f ask=%.2f orders=%llu fills=%llu pos=%lld gaps=%llu malformed=%llu\n",
                   static_cast<unsigned long long>(eng.st.messages), static_cast<unsigned long long>(eng.st.updates),
                   eng.bids.best_price() / 100.0, eng.asks.best_price() / 100.0,
                   static_cast<unsigned long long>(eng.orders), static_cast<unsigned long long>(fills),
                   static_cast<long long>(eng.position), static_cast<unsigned long long>(eng.gaps),
                   static_cast<unsigned long long>(eng.st.malformed));
      tel.stats(static_cast<std::int64_t>(eng.st.messages), static_cast<std::int64_t>(eng.orders),
                static_cast<std::int64_t>(fills), static_cast<std::int64_t>(eng.rejects), eng.position);
      last_print = now;
    }
    if (dur && now - start > dur) break;
  }
  std::fprintf(stderr, "final: msgs=%llu updates=%llu snapshots=%llu trades=%llu orders=%llu fills=%llu gaps=%llu malformed=%llu\n",
               static_cast<unsigned long long>(eng.st.messages), static_cast<unsigned long long>(eng.st.updates),
               static_cast<unsigned long long>(eng.st.snapshots), static_cast<unsigned long long>(eng.st.trades),
               static_cast<unsigned long long>(eng.orders), static_cast<unsigned long long>(fills),
               static_cast<unsigned long long>(eng.gaps), static_cast<unsigned long long>(eng.st.malformed));
  for (int i = 0; i < cNum; ++i) {
    const auto& hh = eng.h[i];
    if (!hh.count()) continue;
    std::fprintf(stderr, "  %-22s n=%-8llu p50=%-7llu p99=%-8llu p99.9=%-8llu max=%llu ns\n", kCbNames[i],
                 static_cast<unsigned long long>(hh.count()), static_cast<unsigned long long>(clk.to_ns(hh.percentile(50))),
                 static_cast<unsigned long long>(clk.to_ns(hh.percentile(99))),
                 static_cast<unsigned long long>(clk.to_ns(hh.percentile(99.9))),
                 static_cast<unsigned long long>(clk.to_ns(hh.max())));
  }
  if (h_k2s.count())
    std::fprintf(stderr, "  %-22s n=%-8llu p50=%-7llu p99=%-8llu p99.9=%-8llu max=%llu ns\n", "kernel_rx_to_send",
                 static_cast<unsigned long long>(h_k2s.count()), static_cast<unsigned long long>(h_k2s.percentile(50)),
                 static_cast<unsigned long long>(h_k2s.percentile(99)),
                 static_cast<unsigned long long>(h_k2s.percentile(99.9)), static_cast<unsigned long long>(h_k2s.max()));
  if (!a.str("json").empty()) {
    std::ofstream f(a.str("json"));
    f << "{\"bench\":\"coinbase_live\",\"product\":\"" << product << "\",\"cpu\":\"" << cpu_model()
      << "\",\"messages\":" << eng.st.messages << ",\"updates\":" << eng.st.updates << ",\"orders\":" << eng.orders
      << ",\"stages\":{";
    for (int i = 0; i < cNum; ++i) f << (i ? "," : "") << "\"" << kCbNames[i] << "\":" << hist_json(eng.h[i], &clk);
    f << ",\"kernel_rx_to_send\":" << hist_json(h_k2s, nullptr) << "}}\n";
  }
  return 0;
}
