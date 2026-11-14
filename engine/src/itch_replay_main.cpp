// Replays an ITCH stream as MoldUDP64 over UDP: first to the paper exchange's feed
// port, then to the engine. Source is a NASDAQ binary file (length-prefixed, as
// published, after gunzip) or a seeded synthetic day.
//
//   itch_replay [--file day.itch] [--messages N] [--symbols K] [--seed S]
//               [--engine 127.0.0.1:31001] [--exchange-feed 127.0.0.1:31000]
//               [--rate 100000] (packets/s, 0 = as fast as possible)
//               [--msgs-per-packet 1] [--cpu 6] [--loops 1] [--only-locates 1,2]
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

#include "lle/args.hpp"
#include "lle/itch_writer.hpp"
#include "lle/net.hpp"
#include "lle/report.hpp"
#include "lle/safety.hpp"
#include "lle/tsc.hpp"

using namespace lle;

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  install_stop_handler();
  const Args a(argc, argv);
  if (a.num("cpu", -1) >= 0) pin_to_cpu(static_cast<int>(a.num("cpu")));

  std::vector<unsigned char> stream;
  if (!a.str("file").empty()) {
    std::ifstream f(a.str("file"), std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", a.str("file").c_str()); return 1; }
    stream.assign(std::istreambuf_iterator<char>(f), {});
  } else {
    itch::SynthConfig c;
    c.messages = static_cast<std::uint64_t>(a.num("messages", 1'000'000));
    c.symbols = static_cast<int>(a.num("symbols", 4));
    c.seed = static_cast<std::uint64_t>(a.num("seed", 42));
    stream = itch::synth_day(c);
  }
  // Optional filter: keep only messages for some locates (plus system events), to
  // shrink a full-market file to the tracked symbols.
  const std::string only = a.str("only-locates");
  if (!only.empty()) {
    std::vector<bool> keep(65536, false);
    for (auto l : a.locates("only-locates", 0)) keep[l] = true;
    std::vector<unsigned char> out;
    std::size_t off = 0;
    while (off + 2 <= stream.size()) {
      const std::size_t len = load_be<std::uint16_t>(stream.data() + off);
      if (off + 2 + len > stream.size()) break;
      const unsigned char* m = stream.data() + off + 2;
      if (len >= 3 && (m[0] == 'S' || keep[load_be<std::uint16_t>(m + 1)]))
        out.insert(out.end(), stream.begin() + static_cast<long>(off), stream.begin() + static_cast<long>(off + 2 + len));
      off += 2 + len;
    }
    stream.swap(out);
  }
  const auto pkts = itch::to_mold_packets(stream, 1400, static_cast<int>(a.num("msgs-per-packet", 1)));
  net::Sender to_engine(net::parse_hostport(a.str("engine", "127.0.0.1:31001")));
  std::unique_ptr<net::Sender> to_exch;
  if (a.str("exchange-feed", "127.0.0.1:31000") != "none")
    to_exch = std::make_unique<net::Sender>(net::parse_hostport(a.str("exchange-feed", "127.0.0.1:31000")));

  const TscClock clk = TscClock::calibrate(100);
  const double rate = a.dbl("rate", 100000);
  const std::uint64_t gap = rate > 0 ? static_cast<std::uint64_t>(clk.ticks_per_ns * 1e9 / rate) : 0;
  const int loops = static_cast<int>(a.num("loops", 1));
  std::fprintf(stderr, "itch_replay: %zu packets x %d loops at %s pkt/s\n", pkts.size(), loops,
               rate > 0 ? std::to_string(static_cast<long long>(rate)).c_str() : "max");
  std::uint64_t sent = 0, drops = 0;
  const std::uint64_t t0 = rdtsc();
  std::uint64_t next = t0;
  for (int l = 0; l < loops && !g_stop; ++l) {
    for (const auto& p : pkts) {
      if (g_stop) break;
      if (gap) {
        while (rdtsc_unordered() < next) cpu_relax();
        next += gap;
      }
      if (to_exch && !to_exch->send(p.data(), p.size())) drops++;
      if (!to_engine.send(p.data(), p.size())) drops++;
      sent++;
    }
  }
  const double secs = static_cast<double>(clk.to_ns(rdtsc() - t0)) / 1e9;
  std::fprintf(stderr, "itch_replay: sent %llu packets in %.2fs (%.0f pkt/s), send failures %llu\n",
               static_cast<unsigned long long>(sent), secs, static_cast<double>(sent) / secs,
               static_cast<unsigned long long>(drops));
  return 0;
}
