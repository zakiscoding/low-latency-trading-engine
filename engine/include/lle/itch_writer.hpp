#pragma once
// ITCH 5.0 encoders and a seeded synthetic order-flow generator.
//
// The generator exists because raw exchange data can't be committed to a public
// repo: tests and CI replay a synthetic day instead, and real NASDAQ sample files
// are replayed locally when present. The flow is a random walk with adds near the
// touch (geometric distance), cancels, partial executions at the touch and
// replaces, in roughly the proportions seen in real ITCH (adds+deletes dominate).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lle/common.hpp"
#include "lle/itch.hpp"

namespace lle::itch {

class Writer {
 public:
  std::vector<unsigned char> buf;  // [u16 len][msg]...

  void system_event(std::uint64_t ts, char code) {
    auto* p = begin('S', 0, ts);
    p[11] = static_cast<unsigned char>(code);
  }
  void directory(std::uint16_t loc, std::uint64_t ts, const char* sym) {
    auto* p = begin('R', loc, ts);
    std::memset(p + 11, ' ', 8);
    std::memcpy(p + 11, sym, std::min<std::size_t>(8, std::strlen(sym)));
    p[19] = 'Q';
  }
  void add(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, Side s, std::uint32_t shares,
           const char* sym, std::uint32_t price) {
    auto* p = begin('A', loc, ts);
    store_be<std::uint64_t>(p + 11, ref);
    p[19] = s == Side::Buy ? 'B' : 'S';
    store_be<std::uint32_t>(p + 20, shares);
    std::memset(p + 24, ' ', 8);
    std::memcpy(p + 24, sym, std::min<std::size_t>(8, std::strlen(sym)));
    store_be<std::uint32_t>(p + 32, price);
  }
  void executed(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares, std::uint64_t match) {
    auto* p = begin('E', loc, ts);
    store_be<std::uint64_t>(p + 11, ref);
    store_be<std::uint32_t>(p + 19, shares);
    store_be<std::uint64_t>(p + 23, match);
  }
  void cancel(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    auto* p = begin('X', loc, ts);
    store_be<std::uint64_t>(p + 11, ref);
    store_be<std::uint32_t>(p + 19, shares);
  }
  void del(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) {
    auto* p = begin('D', loc, ts);
    store_be<std::uint64_t>(p + 11, ref);
  }
  void replace(std::uint16_t loc, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
               std::uint32_t shares, std::uint32_t price) {
    auto* p = begin('U', loc, ts);
    store_be<std::uint64_t>(p + 11, old_ref);
    store_be<std::uint64_t>(p + 19, new_ref);
    store_be<std::uint32_t>(p + 27, shares);
    store_be<std::uint32_t>(p + 31, price);
  }

 private:
  unsigned char* begin(char type, std::uint16_t loc, std::uint64_t ts) {
    const std::size_t len = spec_len(type);
    const std::size_t off = buf.size();
    buf.resize(off + 2 + len, 0);
    unsigned char* p = buf.data() + off;
    store_be<std::uint16_t>(p, static_cast<std::uint16_t>(len));
    p += 2;
    p[0] = static_cast<unsigned char>(type);
    store_be<std::uint16_t>(p + 1, loc);
    store_be<std::uint16_t>(p + 3, 0);
    store_be48(p + 5, ts);
    return p;
  }
};

struct SynthConfig {
  std::uint64_t seed = 42;
  int symbols = 4;
  std::uint64_t messages = 1'000'000;
  std::uint32_t start_price = 1'000'000;  // $100.0000 in ITCH units
  int depth_ticks = 40;                    // adds land within this many ticks of the touch
  std::size_t target_live = 2000;          // resting orders per symbol the flow hovers around
};

// Generates `messages` messages. Locates are 1..symbols.
inline std::vector<unsigned char> synth_day(const SynthConfig& cfg) {
  Writer w;
  std::mt19937_64 rng(cfg.seed);
  struct Live {
    std::uint64_t ref;
    Side side;
    std::uint32_t price;
    std::uint32_t shares;
  };
  struct Sym {
    std::string name;
    std::int64_t mid;  // ticks
    std::vector<Live> live;
  };
  std::vector<Sym> syms(cfg.symbols);
  std::uint64_t ts = 34'200'000'000'000ull;  // 09:30:00
  std::uint64_t next_ref = 1, match = 1;
  w.system_event(ts, 'O');
  for (int i = 0; i < cfg.symbols; ++i) {
    syms[i].name = "SYM" + std::to_string(i);
    syms[i].mid = cfg.start_price / 100 + i * 1000;
    w.directory(static_cast<std::uint16_t>(i + 1), ts, syms[i].name.c_str());
  }
  std::geometric_distribution<int> dist(0.25);
  std::uniform_real_distribution<double> u(0, 1);
  std::uint64_t n = 0;
  while (n < cfg.messages) {
    ts += 1 + (rng() % 2000);
    const int si = static_cast<int>(rng() % cfg.symbols);
    Sym& s = syms[si];
    const std::uint16_t loc = static_cast<std::uint16_t>(si + 1);
    if (u(rng) < 0.002) s.mid += (rng() & 1) ? 1 : -1;  // slow drift
    const double r = u(rng);
    const double p_add = s.live.size() < cfg.target_live ? 0.50 : 0.30;  // mean-reverting depth
    if (s.live.size() < 20 || r < p_add) {
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const int d = std::min(dist(rng), cfg.depth_ticks);
      const std::int64_t px = side == Side::Buy ? s.mid - 1 - d : s.mid + 1 + d;
      const std::uint32_t sh = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
      const std::uint64_t ref = next_ref++;
      w.add(loc, ts, ref, side, sh, s.name.c_str(), static_cast<std::uint32_t>(px * 100));
      s.live.push_back({ref, side, static_cast<std::uint32_t>(px * 100), sh});
    } else if (r < p_add + 0.35) {
      const std::size_t k = rng() % s.live.size();
      if (u(rng) < 0.85) {
        w.del(loc, ts, s.live[k].ref);
        s.live[k] = s.live.back();
        s.live.pop_back();
      } else {
        const std::uint32_t c = std::max<std::uint32_t>(1, s.live[k].shares / 2);
        w.cancel(loc, ts, s.live[k].ref, c);
        s.live[k].shares -= c;
        if (s.live[k].shares == 0) { s.live[k] = s.live.back(); s.live.pop_back(); }
      }
    } else if (r < p_add + 0.47) {
      // Execute against the best order on a random side.
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      std::size_t best = s.live.size();
      for (std::size_t k = 0; k < s.live.size(); ++k) {
        if (s.live[k].side != side) continue;
        if (best == s.live.size() ||
            (side == Side::Buy ? s.live[k].price > s.live[best].price : s.live[k].price < s.live[best].price))
          best = k;
      }
      if (best == s.live.size()) continue;
      const std::uint32_t e = std::min<std::uint32_t>(s.live[best].shares, 100 * (1 + rng() % 3));
      w.executed(loc, ts, s.live[best].ref, e, match++);
      s.live[best].shares -= e;
      if (s.live[best].shares == 0) { s.live[best] = s.live.back(); s.live.pop_back(); }
    } else {
      const std::size_t k = rng() % s.live.size();
      const std::uint64_t nr = next_ref++;
      const std::int64_t delta = static_cast<std::int64_t>(rng() % 3) - 1;
      std::int64_t px = static_cast<std::int64_t>(s.live[k].price / 100) + delta;
      if (s.live[k].side == Side::Buy && px >= s.mid) px = s.mid - 1;
      if (s.live[k].side == Side::Sell && px <= s.mid) px = s.mid + 1;
      const std::uint32_t sh = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
      w.replace(loc, ts, s.live[k].ref, nr, sh, static_cast<std::uint32_t>(px * 100));
      s.live[k] = {nr, s.live[k].side, static_cast<std::uint32_t>(px * 100), sh};
    }
    n++;
  }
  w.system_event(ts + 1, 'C');
  return std::move(w.buf);
}

// Splits a length-prefixed stream into MoldUDP64 packets of up to `max_payload`
// bytes. Each returned packet is a complete datagram.
inline std::vector<std::vector<unsigned char>> to_mold_packets(const std::vector<unsigned char>& stream,
                                                               std::size_t max_payload = 1400,
                                                               int max_msgs = 1) {
  std::vector<std::vector<unsigned char>> out;
  std::uint64_t seq = 1;
  std::size_t off = 0;
  while (off + 2 <= stream.size()) {
    std::vector<unsigned char> pkt(kMoldHeader, 0);
    std::memcpy(pkt.data(), "SYNTH00001", 10);
    store_be<std::uint64_t>(pkt.data() + 10, seq);
    std::uint16_t count = 0;
    while (off + 2 <= stream.size() && count < max_msgs) {
      const std::size_t len = load_be<std::uint16_t>(stream.data() + off);
      if (pkt.size() + 2 + len > max_payload && count > 0) break;
      pkt.insert(pkt.end(), stream.begin() + static_cast<long>(off), stream.begin() + static_cast<long>(off + 2 + len));
      off += 2 + len;
      count++;
    }
    store_be<std::uint16_t>(pkt.data() + 18, count);
    seq += count;
    out.push_back(std::move(pkt));
  }
  return out;
}

}  // namespace lle::itch
