// Paper exchange process. Receives the same market data as the engine (its own
// copy, sent first by the replayer, as an exchange would see its own book before
// subscribers), receives orders, and fills them with PaperExchange's queue model.
// Execution reports go back to whoever sent the order.
//
//   paper_exchange --feed 127.0.0.1:31000 --orders 127.0.0.1:31002 --locates 1,2,3,4
//                  [--latency-us 20] [--cpu 4] [--stats-json out.json]
//   paper_exchange --source bitstamp|coinbase --product BTC-USD --orders 127.0.0.1:31012 --feed 127.0.0.1:31010
//                  [--taker-bp100 6000] [--maker-bp100 4000] [--latency-us 20000]
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <memory>

#include "lle/args.hpp"
#include "lle/coinbase.hpp"
#include "lle/venue.hpp"
#include "lle/ws.hpp"
#include "lle/itch.hpp"
#include "lle/net.hpp"
#include "lle/paper_exchange.hpp"
#include "lle/report.hpp"
#include "lle/safety.hpp"

using namespace lle;

namespace {
struct Ctx {
  int fd;
  sockaddr_in to{};
  bool have_peer = false;
  std::uint64_t reports = 0;
};
void send_report(void* c, const ExecReport& r0, int inst) {
  auto* ctx = static_cast<Ctx*>(c);
  if (!ctx->have_peer) return;
  ExecReport r = r0;
  r.instrument = static_cast<std::uint32_t>(inst < 0 ? 0 : inst);
  unsigned char b[kExecMsgSize];
  r.encode(b);
  ::sendto(ctx->fd, b, sizeof(b), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&ctx->to), sizeof(ctx->to));
  ctx->reports++;
}

// Coinbase L2/trades -> the exchange's mirror book (instrument 0).
struct CbMirror {
  PaperExchange& x;
  void on_sequence(std::int64_t) {}
  void on_l2_begin(std::string_view, bool snap) {
    if (snap) { x.mkt_bids(0).clear(); x.mkt_asks(0).clear(); }
  }
  void on_l2(const coinbase::L2Update& u) { x.on_market_level(0, u.bid ? Side::Buy : Side::Sell, u.price, u.qty); }
  void on_l2_end() {}
  void on_trade(std::string_view, const coinbase::TradePrint& t) {
    const auto a = x.mkt_asks(0).best_price(), b = x.mkt_bids(0).best_price();
    const Side resting = (a != AskLadder::kNone && t.price >= a) ? Side::Sell
                         : (b != BidLadder::kNone && t.price <= b) ? Side::Buy
                         : (t.buy ? Side::Sell : Side::Buy);
    x.on_market_trade(0, resting, t.price, t.qty);
  }
  void on_heartbeat() {}
};

std::pair<std::string, int> split(const std::string& hp) {
  const auto c = hp.rfind(':');
  return {hp.substr(0, c), std::stoi(hp.substr(c + 1))};
}
}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  const auto locs = a.locates("locates", 4);
  if (a.num("cpu", -1) >= 0) pin_to_cpu(static_cast<int>(a.num("cpu")));

  auto [fh, fp] = split(a.str("feed", "127.0.0.1:31000"));
  auto [oh, op] = split(a.str("orders", "127.0.0.1:31002"));
  const int feed_fd = net::bind_udp(fh, fp);
  const int ord_fd = net::bind_udp(oh, op);
  net::set_rcvbuf(feed_fd, 8 << 20);
  net::set_rcvbuf(ord_fd, 4 << 20);
  net::set_nonblocking(feed_fd);
  net::set_nonblocking(ord_fd);

  PaperConfig pc;
  pc.latency_ns = a.num("latency-us", 20) * 1000;
  // Nasdaq's published standard rates (approximate; tiers vary): remove $0.0030/share,
  // add earns a rebate of about $0.0020/share.
  const std::string source = a.str("source", "itch");
  const bool cb = source != "itch";  // a live crypto venue
  if (!cb) {
    pc.fees.kind = FeeModel::PerUnit;
    pc.fees.taker = a.num("taker-micros", 3000);
    pc.fees.maker = a.num("maker-micros", -2000);
  } else {
    // Entry-tier fees, in hundredths of a basis point. Flags, because they change:
    //  coinbase: sources disagree (0.40%/0.60% vs 0.50%/0.90% after a Sept 2026
    //            change); default is the lower, older schedule.
    //  bitstamp: entry tier 0.30% maker / 0.40% taker (check bitstamp.net/fee-schedule).
    const bool bs = source == "bitstamp";
    pc.fees.kind = FeeModel::Bps;
    pc.fees.taker = a.num("taker-bp100", bs ? 4000 : 6000);
    pc.fees.maker = a.num("maker-bp100", bs ? 3000 : 4000);
    pc.qty_scale = 100'000'000;
    pc.levels_log2 = 18;
    pc.max_order_qty = 100'000'000;
  }
  Ctx ctx{ord_fd};
  PaperExchange x(pc, cb ? 1 : static_cast<int>(locs.size()), &send_report, &ctx);
  ItchPaperFeed feed(x, locs);
  std::unique_ptr<ws::Client> cbc;
  CbMirror mirror{x};
  coinbase::Stats cst;
  std::unique_ptr<VenueFeed> vf;
  if (cb) {
    cbc = std::make_unique<ws::Client>();
    vf = std::make_unique<VenueFeed>(parse_venue(source), a.str("product", "BTC-USD"));
    vf->connect(*cbc);
  }

  std::fprintf(stderr, "paper_exchange: feed=%s orders=%s books=%zu latency=%lldus (PAPER ONLY, no routing)\n",
               a.str("feed", "127.0.0.1:31000").c_str(), a.str("orders", "127.0.0.1:31002").c_str(), locs.size(),
               static_cast<long long>(pc.latency_ns / 1000));

  unsigned char buf[2048];
  itch::ParseStats st;
  std::uint64_t feed_pkts = 0, orders = 0;
  std::int64_t last_print = clock_ns(CLOCK_MONOTONIC);
  while (!g_stop) {
    bool any = false;
    if (cbc && !cbc->poll([&](const unsigned char* m, std::size_t len, std::int64_t) {
          vf->parse(m, len, mirror, cst);
          feed_pkts++;
        })) {
      std::fprintf(stderr, "paper_exchange: coinbase connection closed\n");
      break;
    }
    ssize_t n = ::recv(feed_fd, buf, sizeof(buf), 0);
    if (n > 0) {
      itch::parse_mold(buf, static_cast<std::size_t>(n), feed, st);
      feed_pkts++;
      any = true;
    }
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    n = ::recvfrom(ord_fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    const std::int64_t now = clock_ns(CLOCK_MONOTONIC);
    if (n >= static_cast<ssize_t>(kOrderMsgSize)) {
      ctx.to = from;
      ctx.have_peer = true;
      x.submit(OrderMsgView{buf}, now);
      orders++;
      any = true;
    }
    x.tick(now);
    if (!any && now - last_print > 5'000'000'000ll) {
      const auto& ac = x.account();
      std::fprintf(stderr, "paper_exchange: feed_pkts=%llu orders=%llu fills=%llu (maker %llu taker %llu) "
                           "pnl=$%.2f fees=$%.2f pos0=%lld\n",
                   static_cast<unsigned long long>(feed_pkts), static_cast<unsigned long long>(orders),
                   static_cast<unsigned long long>(ac.fills), static_cast<unsigned long long>(ac.maker_fills),
                   static_cast<unsigned long long>(ac.taker_fills), x.pnl_micros() / 1e6, ac.fees_micros / 1e6,
                   static_cast<long long>(ac.position[0]));
      last_print = now;
    }
  }
  {
    const auto& ac = x.account();
    std::fprintf(stderr, "paper_exchange final: feed_pkts=%llu orders=%llu fills=%llu (maker %llu taker %llu) "
                         "rejects=%llu pnl=$%.2f fees=$%.2f malformed=%llu\n",
                 static_cast<unsigned long long>(feed_pkts), static_cast<unsigned long long>(orders),
                 static_cast<unsigned long long>(ac.fills), static_cast<unsigned long long>(ac.maker_fills),
                 static_cast<unsigned long long>(ac.taker_fills), static_cast<unsigned long long>(ac.rejects),
                 x.pnl_micros() / 1e6, ac.fees_micros / 1e6, static_cast<unsigned long long>(st.malformed));
  }
  if (!a.str("stats-json").empty()) {
    const auto& ac = x.account();
    std::ofstream f(a.str("stats-json"));
    f << "{\"orders\":" << ac.orders << ",\"fills\":" << ac.fills << ",\"maker_fills\":" << ac.maker_fills
      << ",\"taker_fills\":" << ac.taker_fills << ",\"rejects\":" << ac.rejects << ",\"pnl_usd\":" << x.pnl_micros() / 1e6
      << ",\"fees_usd\":" << ac.fees_micros / 1e6 << ",\"volume_usd\":" << ac.volume_micros / 1e6 << "}\n";
  }
  ::close(feed_fd);
  ::close(ord_fd);
  return 0;
}
