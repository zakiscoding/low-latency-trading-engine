// Paper exchange process. Receives the same market data as the engine (its own
// copy, sent first by the replayer, as an exchange would see its own book before
// subscribers), receives orders, and fills them with PaperExchange's queue model.
// Execution reports go back to whoever sent the order.
//
//   paper_exchange --feed 127.0.0.1:31000 --orders 127.0.0.1:31002 --locates 1,2,3,4
//                  [--latency-us 20] [--fees nasdaq] [--cpu 4] [--stats-json out.json]
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "lle/args.hpp"
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
  pc.fees.kind = FeeModel::PerUnit;
  pc.fees.taker = a.num("taker-micros", 3000);
  pc.fees.maker = a.num("maker-micros", -2000);
  Ctx ctx{ord_fd};
  PaperExchange x(pc, static_cast<int>(locs.size()), &send_report, &ctx);
  ItchPaperFeed feed(x, locs);

  std::fprintf(stderr, "paper_exchange: feed=%s orders=%s books=%zu latency=%lldus (PAPER ONLY, no routing)\n",
               a.str("feed", "127.0.0.1:31000").c_str(), a.str("orders", "127.0.0.1:31002").c_str(), locs.size(),
               static_cast<long long>(pc.latency_ns / 1000));

  unsigned char buf[2048];
  itch::ParseStats st;
  std::uint64_t feed_pkts = 0, orders = 0;
  std::int64_t last_print = clock_ns(CLOCK_MONOTONIC);
  while (!g_stop) {
    bool any = false;
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
                         "rejects=%llu pnl=$%.2f fees=$%.2f malformed=%llu
",
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
