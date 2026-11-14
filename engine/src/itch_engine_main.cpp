// The benchmark engine: ITCH (MoldUDP64) in over UDP, orders out over UDP to the
// paper exchange. One pinned thread does everything on the hot path.
//
//   itch_engine --listen 127.0.0.1:31001 --exchange 127.0.0.1:31002 --locates 1,2,3,4
//               [--cpu 2] [--rx spin|blocking|uring|xdp] [--busy-poll-us 50]
//               [--xdp-if eth0 --xdp-queue 0 --xdp-mode skb|drv|zerocopy]
//               [--shm /dev/shm/lle-telemetry] [--control /dev/shm/lle-control]
//               [--duration-s 0] [--json out.json] [--no-timing]
//
// Latency reported:
//  - per-stage rdtsc histograms (parse, book, decide, build, send) and the in-process
//    tick-to-trade: packet in user space -> send() returned
//  - kernel_rx_to_send: SO_TIMESTAMPING software RX stamp -> send() returned
//  - kernel_rx_to_tx:   SO_TIMESTAMPING RX stamp -> TX software stamp (when available)
// With --rx xdp the "kernel RX" stamp is taken at the XDP hook instead (earlier).
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>

#include "lle/args.hpp"
#include "lle/net.hpp"
#include "lle/pipeline.hpp"
#include "lle/report.hpp"
#include "lle/safety.hpp"
#include "lle/shm.hpp"
#include "lle/telemetry_sink.hpp"
#include "lle/xdp.hpp"

using namespace lle;

namespace {

void apply_control(shm::ControlBlock* cb, ItchEngine<MarketBook, net::Sender, PreserializedBuilder, ShmTelemetry>& eng,
                   const TscClock& clk) {
  if (cb->kill.load()) eng.risk().kill(); else eng.risk().unkill();
  eng.set_trading_enabled(cb->trading_enabled.load() != 0);
  RiskLimits rl = eng.risk().limits();
  if (cb->max_order_qty > 0) rl.max_order_qty = cb->max_order_qty;
  if (cb->max_position > 0) rl.max_position = cb->max_position;
  if (cb->max_notional_ticks > 0) rl.max_notional_ticks = cb->max_notional_ticks;
  if (cb->max_orders_per_sec > 0) rl.max_orders_per_sec = static_cast<std::uint32_t>(cb->max_orders_per_sec);
  if (cb->max_orders_per_day > 0) rl.max_orders_per_day = static_cast<std::uint32_t>(cb->max_orders_per_day);
  if (cb->max_daily_loss_ticks > 0) rl.max_daily_loss_ticks = cb->max_daily_loss_ticks;
  eng.risk().set_limits(rl, clk.ticks_per_ns);
  StrategyParams sp = eng.strategy().params();
  if (cb->imbalance_num > 0 && cb->imbalance_den > 0) {
    sp.imbalance_num = cb->imbalance_num;
    sp.imbalance_den = cb->imbalance_den;
  }
  if (cb->order_qty > 0) sp.order_qty = cb->order_qty;
  eng.strategy().set_params(sp);
  std::fprintf(stderr, "control: kill=%u trading=%u max_qty=%lld max_pos=%lld\n", cb->kill.load(),
               cb->trading_enabled.load(), static_cast<long long>(rl.max_order_qty),
               static_cast<long long>(rl.max_position));
}

// Either the socket receiver or the AF_XDP one. The branch is fixed for the whole
// run, so it is perfectly predicted.
struct AnyRx {
  std::unique_ptr<net::Receiver> sock;
  std::unique_ptr<xdp::XdpReceiver> xsk;
  LLE_ALWAYS_INLINE bool poll(net::Packet& p) noexcept { return xsk ? xsk->poll(p) : sock->poll(p); }
};

}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  const auto listen = a.str("listen", "127.0.0.1:31001");
  const auto exch = a.str("exchange", "127.0.0.1:31002");
  safety::require_allowed("udp://" + exch);
  const auto locs = a.locates("locates", 4);
  const int cpu = static_cast<int>(a.num("cpu", -1));
  const std::string rx_name = a.str("rx", "spin");
  const bool use_xdp = rx_name == "xdp";
  const net::RxMode mode = use_xdp ? net::RxMode::Spin : net::parse_mode(rx_name);

  if (cpu >= 0 && !pin_to_cpu(cpu)) std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
  const bool locked = lock_memory();
  const TscClock clk = TscClock::calibrate(200);

  const auto la = net::parse_hostport(listen);
  char host[32];
  inet_ntop(AF_INET, &la.sin_addr, host, sizeof(host));
  const int rxfd = net::bind_udp(host, ntohs(la.sin_port));
  net::set_rcvbuf(rxfd, 8 << 20);
  const bool rx_ts = net::enable_timestamps(rxfd, false);
  bool busy = false;
  if (a.num("busy-poll-us", 0) > 0) busy = net::enable_busy_poll(rxfd, static_cast<int>(a.num("busy-poll-us")));
  AnyRx rx;
  std::string rx_desc = rx_name;
  if (use_xdp) {
    xdp::Config xc;
    xc.ifname = a.str("xdp-if", "eth0");
    xc.queue = static_cast<int>(a.num("xdp-queue", 0));
    xc.udp_port = ntohs(la.sin_port);
    xc.mode = xdp::parse_mode(a.str("xdp-mode", "skb"));
    rx.xsk = std::make_unique<xdp::XdpReceiver>(xc);
    rx_desc = std::string("xdp/") + rx.xsk->mode_name() + "@" + xc.ifname + ":" + std::to_string(xc.queue);
  } else {
    rx.sock = std::make_unique<net::Receiver>(rxfd, mode);
  }
  net::Sender tx(net::parse_hostport(exch), /*tx_timestamps=*/true);
  net::set_nonblocking(tx.fd());

  BookConfig bc;
  MarketBook book(bc);
  for (auto l : locs) book.add_instrument(l);

  const std::string shm_path = a.str("shm", "/dev/shm/lle-telemetry");
  shm::TelemetryRing ring(shm_path, 16);
  ShmTelemetry tel(ring, clk);
  tel.set_top_rate_hz(50);
  std::unique_ptr<shm::Control> ctrl;
  if (!a.str("control").empty() || a.flag("control")) ctrl = std::make_unique<shm::Control>(a.str("control", "/dev/shm/lle-control"), true);

  StrategyParams sp;
  sp.order_qty = a.num("order-qty", 10);
  sp.cooldown_cycles = static_cast<std::uint64_t>(clk.ticks_per_ns * 1e3 * static_cast<double>(a.num("cooldown-us", 0)));
  RiskLimits rl;
  rl.max_orders_per_sec = static_cast<std::uint32_t>(a.num("max-orders-per-sec", 1000));
  rl.max_orders_per_day = static_cast<std::uint32_t>(a.num("max-orders-per-day", 1'000'000));
  rl.max_position = a.num("max-position", 1000);
  ItchEngine<MarketBook, net::Sender, PreserializedBuilder, ShmTelemetry> eng(book, tx, tel, sp, rl, clk);
  if (ctrl) apply_control(ctrl->get(), eng, clk);

  std::fprintf(stderr,
               "itch_engine: listen=%s exchange=%s books=%zu rx=%s rx_ts=%d busy_poll=%d mlock=%d cpu=%d tsc=%.3fGHz\n",
               listen.c_str(), exch.c_str(), locs.size(), rx_desc.c_str(), rx_ts, busy, locked, cpu,
               clk.ticks_per_ns);

  Histogram h_kernel_to_send, h_kernel_to_tx, h_kernel_to_user;
  // Kernel RX stamp of the packet that triggered the n-th send (TX OPT_ID counter).
  std::array<std::int64_t, 4096> rx_of_send{};
  std::uint32_t sends = 0;

  const std::uint64_t one_sec = static_cast<std::uint64_t>(clk.ticks_per_ns * 1e9);
  const std::uint64_t snap_gap = one_sec / 20;
  const std::uint64_t start = rdtsc();
  const std::uint64_t dur = static_cast<std::uint64_t>(a.num("duration-s", 0)) * one_sec;
  std::uint64_t last_snap = 0, last_stats = start;
  std::uint64_t packets = 0, fills = 0, idle_since = 0;
  unsigned char exec_buf[256];

  while (!g_stop) {
    net::Packet pkt;
    if (LLE_LIKELY(rx.poll(pkt))) {
      const std::uint64_t t_rx = rdtsc();
      const std::uint64_t before = eng.counters().orders;
      eng.begin_packet(t_rx);
      eng.on_packet(pkt.data, pkt.len);
      packets++;
      if (eng.counters().orders != before) {
        const std::int64_t done = clock_ns(CLOCK_REALTIME);  // vDSO, after the order is out
        std::int64_t wire = 0;
        if (pkt.kernel_rx_ns) {
          wire = done - pkt.kernel_rx_ns;
          h_kernel_to_send.record(static_cast<std::uint64_t>(wire > 0 ? wire : 0));
          rx_of_send[sends & 4095] = pkt.kernel_rx_ns;
        }
        sends += static_cast<std::uint32_t>(eng.counters().orders - before);
        tel.flush_latency(wire);
      }
      // Kernel RX stamp -> user space, sampled 1 in 16 packets (a vDSO clock read
      // costs ~20 ns, so it is kept off most packets; done after the work above).
      if (pkt.kernel_rx_ns && (packets & 15) == 0) {
        const std::int64_t d = clock_ns(CLOCK_REALTIME) - static_cast<std::int64_t>(clk.to_ns(rdtsc_unordered() - t_rx)) -
                               pkt.kernel_rx_ns;
        if (d > 0) h_kernel_to_user.record(static_cast<std::uint64_t>(d));
      }
      idle_since = 0;
      continue;
    }
    // ---- idle path: everything that isn't market data ----
    const std::uint64_t now = rdtsc_unordered();
    if (!idle_since) idle_since = now;
    for (;;) {
      const ssize_t n = ::recv(tx.fd(), exec_buf, sizeof(exec_buf), MSG_DONTWAIT);
      if (n < static_cast<ssize_t>(kExecMsgSize)) break;
      const ExecReport r = ExecReport::decode(exec_buf);
      eng.on_exec_report(r, static_cast<int>(r.instrument));
      if (r.type == 'F') {
        fills++;
        tel.on_fill(static_cast<int>(r.instrument), r);
      }
    }
    tx.drain_tx_timestamps([&](std::uint32_t id, std::int64_t ts) {
      const std::int64_t k = rx_of_send[id & 4095];
      if (k && ts > k && id < sends) h_kernel_to_tx.record(static_cast<std::uint64_t>(ts - k));
    });
    if (ctrl && ctrl->changed()) apply_control(ctrl->get(), eng, clk);
    if (now - last_snap > snap_gap) {
      tel.snapshot(book, 20);
      last_snap = now;
    }
    if (now - last_stats > one_sec / 4) {
      tel.stats(static_cast<std::int64_t>(eng.counters().messages), static_cast<std::int64_t>(eng.counters().orders),
                static_cast<std::int64_t>(fills), static_cast<std::int64_t>(eng.counters().risk_rejects), eng.position(0));
      if (rx.xsk) rx.xsk->resync_clock();
      last_stats = now;
    }
    if (dur && now - start > dur) break;
    if (mode == net::RxMode::Blocking) continue;
  }

  const auto& c = eng.counters();
  std::fprintf(stderr, "packets=%llu messages=%llu top_changes=%llu orders=%llu rejects=%llu fills=%llu malformed=%llu\n",
               static_cast<unsigned long long>(packets), static_cast<unsigned long long>(c.messages),
               static_cast<unsigned long long>(c.top_changes), static_cast<unsigned long long>(c.orders),
               static_cast<unsigned long long>(c.risk_rejects), static_cast<unsigned long long>(fills),
               static_cast<unsigned long long>(eng.parse_stats().malformed));
  print_stages(eng.hists(), clk, stderr);
  auto line = [](const char* n, const Histogram& h) {
    if (!h.count()) return;
    std::fprintf(stderr, "  %-18s %8llu p50=%llu p99=%llu p99.9=%llu max=%llu (ns)\n", n,
                 static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(h.percentile(50)),
                 static_cast<unsigned long long>(h.percentile(99)), static_cast<unsigned long long>(h.percentile(99.9)),
                 static_cast<unsigned long long>(h.max()));
  };
  line("kernel_rx_to_user", h_kernel_to_user);
  line("kernel_rx_to_send", h_kernel_to_send);
  line("kernel_rx_to_tx", h_kernel_to_tx);

  if (!a.str("json").empty()) {
    std::ofstream f(a.str("json"));
    f << "{\"bench\":\"udp_tick_to_trade\",\"label\":\"" << a.str("label") << "\",\"cpu\":\"" << cpu_model()
      << "\",\"rx\":\"" << rx_desc << "\",\"busy_poll\":" << busy << ",\"pinned_cpu\":" << cpu
      << ",\"packets\":" << packets << ",\"orders\":" << c.orders << ",\"stages\":" << stages_json(eng.hists(), clk, true)
      << ",\"kernel_rx_to_user\":" << hist_json(h_kernel_to_user, nullptr)
      << ",\"kernel_rx_to_send\":" << hist_json(h_kernel_to_send, nullptr)
      << ",\"kernel_rx_to_send_buckets\":" << hist_buckets_json(h_kernel_to_send, nullptr)
      << ",\"kernel_rx_to_tx\":" << hist_json(h_kernel_to_tx, nullptr) << "}\n";
  }
  ::close(rxfd);
  return 0;
}
