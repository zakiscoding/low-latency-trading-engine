// Records live Coinbase market data into QuestDB (InfluxDB line protocol over TCP).
// Runs as its own process on its own core; it never touches the engine.
// The stored data is for private research only (Coinbase's market data terms
// don't allow redistributing it), so the tick store is never exposed publicly.
//
//   recorder --venue coinbase|bitstamp --product BTC-USD [--questdb 127.0.0.1:9009] [--duration-s 0]
//
// Tables (created by QuestDB on first write):
//   l2(venue SYMBOL, product SYMBOL, side SYMBOL, price DOUBLE, qty DOUBLE, snapshot BOOLEAN, ts)
//   trades(venue SYMBOL, product SYMBOL, side SYMBOL, price DOUBLE, size DOUBLE, ts)
// ts is the local receive time (kernel RX stamp when available).
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "lle/args.hpp"
#include "lle/coinbase.hpp"
#include "lle/safety.hpp"
#include "lle/tsc.hpp"
#include "lle/venue.hpp"
#include "lle/ws.hpp"

using namespace lle;

namespace {

class Ilp {
 public:
  explicit Ilp(const std::string& hostport) {
    const auto c = hostport.rfind(':');
    const std::string host = hostport.substr(0, c);
    const int port = std::stoi(hostport.substr(c + 1));
    safety::require_allowed("tcp://" + host);
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
      throw std::runtime_error("cannot connect to QuestDB ILP at " + hostport);
    buf_.reserve(1 << 20);
  }
  ~Ilp() { flush(); ::close(fd_); }
  std::string& buf() { return buf_; }
  void maybe_flush() { if (buf_.size() > (512u << 10)) flush(); }
  void flush() {
    std::size_t off = 0;
    while (off < buf_.size()) {
      const ssize_t n = ::send(fd_, buf_.data() + off, buf_.size() - off, MSG_NOSIGNAL);
      if (n <= 0) throw std::runtime_error("QuestDB connection lost");
      off += static_cast<std::size_t>(n);
    }
    lines_flushed_ += lines_;
    lines_ = 0;
    buf_.clear();
  }
  void line_done() { lines_++; }
  std::uint64_t flushed() const { return lines_flushed_; }

 private:
  int fd_ = -1;
  std::string buf_;
  std::uint64_t lines_ = 0, lines_flushed_ = 0;
};

// Fixed point -> decimal text without going through double.
void put_fixed(std::string& s, std::int64_t v, int dec) {
  char tmp[32];
  const std::int64_t p = [&] { std::int64_t x = 1; for (int i = 0; i < dec; ++i) x *= 10; return x; }();
  std::snprintf(tmp, sizeof(tmp), "%lld.%0*lld", static_cast<long long>(v / p), dec, static_cast<long long>(v % p));
  s += tmp;
}

struct Rec {
  Ilp& out;
  std::string product;
  std::string venue;
  std::int64_t ts = 0;
  bool snap = false;
  std::uint64_t l2 = 0, trades = 0;
  void on_sequence(std::int64_t) {}
  void on_l2_begin(std::string_view p, bool s) { product.assign(p); snap = s; }
  void on_l2(const coinbase::L2Update& u) {
    auto& b = out.buf();
    b += "l2,venue=" + venue + ",product=";
    b += product;
    b += u.bid ? ",side=bid price=" : ",side=offer price=";
    put_fixed(b, u.price, 2);
    b += ",qty=";
    put_fixed(b, u.qty, 8);
    b += snap ? ",snapshot=t " : ",snapshot=f ";
    b += std::to_string(ts);
    b += '\n';
    out.line_done();
    l2++;
  }
  void on_l2_end() {}
  void on_trade(std::string_view p, const coinbase::TradePrint& t) {
    auto& b = out.buf();
    b += "trades,venue=" + venue + ",product=";
    b.append(p.data(), p.size());
    b += t.buy ? ",side=BUY price=" : ",side=SELL price=";
    put_fixed(b, t.price, 2);
    b += ",size=";
    put_fixed(b, t.qty, 8);
    b += ' ';
    b += std::to_string(ts);
    b += '\n';
    out.line_done();
    trades++;
  }
  void on_heartbeat() {}
};

}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  const std::string product = a.str("product", "BTC-USD");
  Ilp out(a.str("questdb", "127.0.0.1:9009"));
  const Venue venue = parse_venue(a.str("venue", "coinbase"));
  VenueFeed feed(venue, product);
  ws::Client c;
  feed.connect(c);
  Rec rec{out, product, venue_name(venue)};
  coinbase::Stats st;
  const std::int64_t start = clock_ns(CLOCK_MONOTONIC);
  const std::int64_t dur = a.num("duration-s", 0) * 1'000'000'000ll;
  std::int64_t last = start;
  while (!g_stop) {
    const bool ok = c.poll([&](const unsigned char* m, std::size_t n, std::int64_t krx) {
      rec.ts = krx ? krx : clock_ns(CLOCK_REALTIME);
      feed.parse(m, n, rec, st);
      out.maybe_flush();
    });
    if (!ok) break;
    const std::int64_t now = clock_ns(CLOCK_MONOTONIC);
    if (now - last > 1'000'000'000ll) {
      out.flush();
      last = now;
    }
    if (dur && now - start > dur) break;
    pollfd p{c.fd(), POLLIN, 0};
    ::poll(&p, 1, 50);  // the recorder doesn't need to spin
  }
  out.flush();
  std::fprintf(stderr, "recorder: messages=%llu l2_rows=%llu trade_rows=%llu written=%llu malformed=%llu\n",
               static_cast<unsigned long long>(st.messages), static_cast<unsigned long long>(rec.l2),
               static_cast<unsigned long long>(rec.trades), static_cast<unsigned long long>(out.flushed()),
               static_cast<unsigned long long>(st.malformed));
  return 0;
}
