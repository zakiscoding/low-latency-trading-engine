// IEX HIST replay: reads a DEEP or TOPS capture (pcap or pcapng, gunzipped) and
//   - builds per-symbol L2 books from DEEP price-level updates in place, timing
//     parse+apply per IEX-TP segment and per message with rdtsc, and/or
//   - re-sends each IEX-TP segment as a UDP datagram (as IEX multicasts it), to
//     drive a receiver (socket or AF_XDP) with real packet sizes.
//
//   iex_replay --file 20240105_IEXTP1_DEEP1.0.pcap [--symbols AAPL,MSFT] [--max-books 16]
//              [--to 127.0.0.1:31001 --rate 100000] [--cpu 2] [--json out.json] [--top 5]
//   iex_replay --synth out.pcap [--messages 1000000] [--symbols-n 8] [--seed 1]
//              writes a seeded synthetic DEEP capture (fuzz seed, benchmark without a download)
//
// The file is mmap'd; nothing is copied. Raw IEX data is not committed to the repo
// (IEX HIST terms: free, attribution required).
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#include "lle/args.hpp"
#include "lle/histogram.hpp"
#include "lle/iex.hpp"
#include "lle/net.hpp"
#include "lle/report.hpp"
#include "lle/safety.hpp"
#include "lle/tsc.hpp"

using namespace lle;

namespace {

struct Handler {
  iex::L2Books* books;
  std::uint64_t applied = 0, consistent_points = 0, quotes = 0, trades = 0;
  std::uint64_t digest = 1469598103934665603ull;
  LLE_ALWAYS_INLINE void on_level(const iex::PriceLevel& u) noexcept {
    if (!books) return;
    if (auto* b = books->apply(u)) {
      applied++;
      if (u.complete) {
        consistent_points++;
        // FNV-1a over the touch after every consistent point: replay determinism.
        const std::int64_t v[2] = {b->bids.empty() ? 0 : b->bids.best_price(), b->asks.empty() ? 0 : b->asks.best_price()};
        const auto* q = reinterpret_cast<const unsigned char*>(v);
        for (int i = 0; i < 16; ++i) digest = (digest ^ q[i]) * 1099511628211ull;
      }
    }
  }
  LLE_ALWAYS_INLINE void on_quote(const iex::Quote&) noexcept { quotes++; }
  LLE_ALWAYS_INLINE void on_trade(const iex::Trade&) noexcept { trades++; }
};

// Random-walk DEEP book per symbol: each message moves one level near the touch;
// batches of 1-4 messages, the last one flagged "complete", 1-8 messages per segment.
int write_synth(const std::string& out, std::uint64_t messages, int nsym, std::uint64_t seed) {
  std::vector<unsigned char> cap;
  iex::pcap_header(cap);
  iex::SegmentWriter w(iex::kProtoDeep);
  std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
  auto rnd = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  std::vector<std::int64_t> mid(static_cast<std::size_t>(nsym));
  std::vector<std::string> names;
  for (int i = 0; i < nsym; ++i) {
    mid[static_cast<std::size_t>(i)] = 1'000'000 + i * 250'000;  // $100, $125, ...
    names.push_back("SYM" + std::to_string(i));
  }
  std::int64_t ts = 1'700'000'000'000'000'000;
  std::uint64_t written = 0;
  while (written < messages) {
    const int per_seg = 1 + static_cast<int>(rnd() % 8);
    for (int k = 0; k < per_seg && written < messages; ++k) {
      const auto s = static_cast<std::size_t>(rnd() % static_cast<std::uint64_t>(nsym));
      if (rnd() % 16 == 0) {
        // Mid moves a cent: the level it moves onto is removed first, so the book
        // never crosses (bids stay below mid, asks above).
        const bool up = rnd() & 1;
        mid[s] += up ? 100 : -100;
        w.level(!up, false, ts, names[s], 0, mid[s]);
        written++;
      }
      const bool bid = rnd() & 1;
      const std::int64_t off = static_cast<std::int64_t>(1 + rnd() % 10) * 100;
      const std::int64_t px = bid ? mid[s] - off : mid[s] + off;
      const std::uint32_t size = (rnd() % 5 == 0) ? 0u : static_cast<std::uint32_t>(100 * (1 + rnd() % 20));
      ts += static_cast<std::int64_t>(rnd() % 20'000);
      w.level(bid, (rnd() % 3) != 0, ts, names[s], size, px);
      written++;
    }
    iex::pcap_udp(cap, ts, w.flush(ts));
  }
  std::ofstream o(out, std::ios::binary);
  o.write(reinterpret_cast<const char*>(cap.data()), static_cast<std::streamsize>(cap.size()));
  std::fprintf(stderr, "wrote %s: %llu messages, %zu bytes\n", out.c_str(), (unsigned long long)written, cap.size());
  return o ? 0 : 1;
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> v;
  std::stringstream ss(s);
  for (std::string x; std::getline(ss, x, ',');)
    if (!x.empty()) v.push_back(x);
  return v;
}

}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  if (!a.str("synth").empty())
    return write_synth(a.str("synth"), static_cast<std::uint64_t>(a.num("messages", 1'000'000)),
                       static_cast<int>(a.num("symbols-n", 8)), static_cast<std::uint64_t>(a.num("seed", 1)));
  const std::string path = a.str("file");
  if (path.empty()) {
    std::fprintf(stderr, "usage: iex_replay --file capture.pcap [--symbols A,B] [--to host:port --rate N] [--json out]\n");
    return 2;
  }
  if (a.num("cpu", -1) >= 0) pin_to_cpu(static_cast<int>(a.num("cpu")));

  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) { std::perror(path.c_str()); return 1; }
  struct stat stt {};
  ::fstat(fd, &stt);
  const std::size_t n = static_cast<std::size_t>(stt.st_size);
  void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
  if (m == MAP_FAILED) { std::perror("mmap"); return 1; }
  ::madvise(m, n, MADV_SEQUENTIAL);
  const auto* base = static_cast<const unsigned char*>(m);
  if (n >= 2 && base[0] == 0x1f && base[1] == 0x8b) {
    std::fprintf(stderr, "%s is gzip-compressed; gunzip it first\n", path.c_str());
    return 1;
  }
  iex::CaptureReader rd(base, n);
  if (!rd.ok()) { std::fprintf(stderr, "not a pcap/pcapng file\n"); return 1; }

  const bool build = !a.flag("no-book");
  std::unique_ptr<iex::L2Books> books;
  if (build) {
    books = std::make_unique<iex::L2Books>(static_cast<std::size_t>(a.num("max-books", 16)));
    const auto syms = split_csv(a.str("symbols"));
    for (const auto& s : syms) books->track(s);
    if (!syms.empty()) books->fix();
  }

  std::unique_ptr<net::Sender> tx;
  const std::string to = a.str("to");
  if (!to.empty()) {
    safety::require_allowed("udp://" + to);
    tx = std::make_unique<net::Sender>(net::parse_hostport(to));
  }
  const double rate = static_cast<double>(a.num("rate", 0));

  const TscClock clk = TscClock::calibrate(100);
  Handler h{books.get()};
  iex::Stats st;
  iex::SeqState seq;
  Histogram h_seg, h_msg;
  std::uint64_t frames = 0, non_udp = 0, sent = 0, send_fail = 0;
  const std::uint64_t gap = rate > 0 ? static_cast<std::uint64_t>(clk.ticks_per_ns * 1e9 / rate) : 0;
  std::uint64_t next_send = rdtsc();
  std::uint16_t proto = 0;

  iex::Frame f;
  while (!g_stop && rd.next(f)) {
    frames++;
    const unsigned char* pl;
    std::size_t pn;
    if (!iex::udp_payload(f.data, f.len, pl, pn)) { non_udp++; continue; }
    if (pn >= iex::kTpHeader && !proto) proto = iex::load_le<std::uint16_t>(pl + 2);
    const std::uint64_t before = st.messages;
    const std::uint64_t t0 = rdtsc();
    iex::parse_segment(pl, pn, h, st, &seq);
    const std::uint64_t t1 = rdtsc();
    const std::uint64_t k = st.messages - before;
    if (k) {
      h_seg.record(clk.to_ns(t1 - t0));
      h_msg.record(clk.to_ns((t1 - t0) / k));
    }
    if (tx) {
      if (gap) {
        while (rdtsc_unordered() < next_send) cpu_relax();
        next_send += gap;
      }
      if (tx->send(pl, pn)) sent++;
      else send_fail++;
    }
  }

  const char* feed = proto == iex::kProtoDeep ? "DEEP" : proto == iex::kProtoTops ? "TOPS" : "unknown";
  std::fprintf(stderr,
               "iex_replay: feed=%s frames=%llu non_udp=%llu segments=%llu messages=%llu levels=%llu quotes=%llu trades=%llu "
               "other=%llu malformed=%llu gaps=%llu\n",
               feed, (unsigned long long)frames, (unsigned long long)non_udp, (unsigned long long)st.segments,
               (unsigned long long)st.messages, (unsigned long long)st.levels, (unsigned long long)st.quotes,
               (unsigned long long)st.trades, (unsigned long long)st.other, (unsigned long long)st.malformed,
               (unsigned long long)st.gaps);
  if (books)
    std::fprintf(stderr, "  books=%zu applied=%llu consistent_points=%llu untracked=%llu subpenny=%llu digest=0x%016llx\n",
                 books->size(), (unsigned long long)h.applied, (unsigned long long)h.consistent_points,
                 (unsigned long long)books->untracked(), (unsigned long long)books->subpenny(), (unsigned long long)h.digest);
  if (tx) std::fprintf(stderr, "  sent=%llu send_fail=%llu\n", (unsigned long long)sent, (unsigned long long)send_fail);
  auto line = [](const char* nm, const Histogram& hh) {
    if (!hh.count()) return;
    std::fprintf(stderr, "  %-16s %10llu p50=%llu p99=%llu p99.9=%llu max=%llu (ns)\n", nm, (unsigned long long)hh.count(),
                 (unsigned long long)hh.percentile(50), (unsigned long long)hh.percentile(99),
                 (unsigned long long)hh.percentile(99.9), (unsigned long long)hh.max());
  };
  line("segment", h_seg);
  line("per_message", h_msg);
  const int top = static_cast<int>(a.num("top", 5));
  if (books) {
    for (std::size_t i = 0; i < books->size() && static_cast<int>(i) < top; ++i) {
      const auto& b = books->book(i);
      const double bid = b.bids.empty() ? 0 : b.bids.best_price() / 100.0;
      const double ask = b.asks.empty() ? 0 : b.asks.best_price() / 100.0;
      std::fprintf(stderr, "  %-8s updates=%-9llu bid=%.2f x %lld  ask=%.2f x %lld\n", iex::symbol_str(b.symbol).c_str(),
                   (unsigned long long)b.updates, bid, b.bids.empty() ? 0ll : (long long)b.bids.best_qty(), ask,
                   b.asks.empty() ? 0ll : (long long)b.asks.best_qty());
    }
  }
  if (!a.str("json").empty()) {
    std::ofstream o(a.str("json"));
    o << "{\"bench\":\"iex_replay\",\"feed\":\"" << feed << "\",\"cpu\":\"" << cpu_model() << "\",\"segments\":" << st.segments
      << ",\"messages\":" << st.messages << ",\"levels\":" << st.levels << ",\"gaps\":" << st.gaps
      << ",\"segment\":" << hist_json(h_seg, nullptr) << ",\"per_message\":" << hist_json(h_msg, nullptr) << "}\n";
  }
  ::munmap(m, n);
  ::close(fd);
  return 0;
}
