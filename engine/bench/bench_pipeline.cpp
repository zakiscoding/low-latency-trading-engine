// In-process pipeline benchmark: replays an ITCH stream (synthetic, or a real
// NASDAQ sample file) through each engine variant on one pinned thread and reports
// per-stage latency. No network, so this isolates the code's own cost.
//
//   bench_pipeline [--file F.itch] [--messages N] [--symbols K] [--locates a,b,c]
//                  [--cpu C] [--variant all|v0..v5] [--json out.json] [--repeat R]
//
// Variants (each changes one thing from the previous):
//   v0  std::map book + allocate-and-decode parse + text order build
//   v1  flat-array book
//   v2  + in-place zero-copy parse
//   v3  + pre-serialised order templates
//   v4  + identity hash for order refs (locality for increasing refs)
//   v5  + transparent huge pages for the tables  (the shipped hot path)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lle/itch_writer.hpp"
#include "lle/pipeline.hpp"
#include "lle/report.hpp"
#include "lle/safety.hpp"

using namespace lle;

namespace {

struct Args {
  std::string file;
  std::uint64_t messages = 5'000'000;
  int symbols = 8;
  std::vector<std::uint16_t> locates;
  int cpu = -1;
  std::string variant = "all";
  std::string json;
  std::string label = "";
  int repeat = 1;
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
    if (k == "--file") a.file = next();
    else if (k == "--messages") a.messages = std::stoull(next());
    else if (k == "--symbols") a.symbols = std::stoi(next());
    else if (k == "--cpu") a.cpu = std::stoi(next());
    else if (k == "--variant") a.variant = next();
    else if (k == "--json") a.json = next();
    else if (k == "--label") a.label = next();
    else if (k == "--repeat") a.repeat = std::stoi(next());
    else if (k == "--locates") {
      std::stringstream ss(next());
      std::string t;
      while (std::getline(ss, t, ',')) a.locates.push_back(static_cast<std::uint16_t>(std::stoi(t)));
    }
  }
  return a;
}

// Index of message offsets so every variant sees exactly the same calls.
struct Stream {
  std::vector<unsigned char> bytes;
  std::vector<std::uint32_t> offs;  // offset of each message body; length at offs-2
};

Stream load(const Args& a) {
  Stream s;
  if (!a.file.empty()) {
    std::ifstream f(a.file, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", a.file.c_str()); std::exit(1); }
    s.bytes.assign(std::istreambuf_iterator<char>(f), {});
  } else {
    itch::SynthConfig c;
    c.messages = a.messages;
    c.symbols = a.symbols;
    s.bytes = itch::synth_day(c);
  }
  std::size_t off = 0;
  while (off + 2 <= s.bytes.size()) {
    const std::size_t len = load_be<std::uint16_t>(s.bytes.data() + off);
    if (off + 2 + len > s.bytes.size()) break;
    s.offs.push_back(static_cast<std::uint32_t>(off + 2));
    off += 2 + len;
    if (!a.file.empty() && s.offs.size() >= a.messages) break;
  }
  return s;
}

struct Outcome {
  std::string name;
  StageHists h;
  double seconds = 0;
  std::uint64_t messages = 0, orders = 0;
};

template <class BookT, class BuilderT, bool kCopy>
Outcome run_variant(const char* name, const Stream& s, const std::vector<std::uint16_t>& locs, const TscClock& clk,
                    OrderIdMap::Hash hash = OrderIdMap::Hash::Fibonacci, bool thp = false) {
  OrderIdMap::default_hash = hash;
  MemoryPolicy::hugepages = thp;
  BookConfig bc;
  BookT book(bc);
  for (auto l : locs) book.add_instrument(l);
  SinkSender tx;
  NullTelemetry tel;
  StrategyParams sp;
  RiskLimits rl;
  rl.max_orders_per_sec = 1'000'000'000;
  rl.max_orders_per_day = 1'000'000'000;
  rl.max_position = 1'000'000'000;
  rl.max_notional_ticks = 1ll << 60;
  sp.position_limit = 1'000'000'000;
  ItchEngine<BookT, SinkSender, BuilderT, NullTelemetry, true, kCopy> eng(book, tx, tel, sp, rl, clk);
  const unsigned char* base = s.bytes.data();
  const std::uint64_t t0 = rdtsc();
  for (std::uint32_t off : s.offs) {
    const std::size_t len = load_be<std::uint16_t>(base + off - 2);
    eng.on_message(base + off, len);
  }
  const std::uint64_t t1 = rdtsc();
  Outcome o;
  o.name = name;
  o.h = eng.hists();
  o.seconds = static_cast<double>(clk.to_ns(t1 - t0)) / 1e9;
  o.messages = s.offs.size();
  o.orders = eng.counters().orders;
  return o;
}

std::vector<std::uint16_t> pick_locates(const Args& a, const Stream& s) {
  if (!a.locates.empty()) return a.locates;
  if (a.file.empty()) {
    std::vector<std::uint16_t> v;
    for (int i = 1; i <= a.symbols; ++i) v.push_back(static_cast<std::uint16_t>(i));
    return v;
  }
  // Real file: track the K locates with the most add-order messages.
  std::vector<std::uint64_t> cnt(65536, 0);
  for (std::uint32_t off : s.offs)
    if (s.bytes[off] == 'A' || s.bytes[off] == 'F') cnt[load_be<std::uint16_t>(&s.bytes[off + 1])]++;
  std::vector<std::uint16_t> v;
  for (int k = 0; k < a.symbols; ++k) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < cnt.size(); ++i)
      if (cnt[i] > cnt[best]) best = i;
    if (!cnt[best]) break;
    v.push_back(static_cast<std::uint16_t>(best));
    cnt[best] = 0;
  }
  return v;
}

}  // namespace

int main(int argc, char** argv, char** envp) {
  safety::enforce_paper_only(envp);
  const Args a = parse(argc, argv);
  if (a.cpu >= 0 && !pin_to_cpu(a.cpu)) std::fprintf(stderr, "warning: could not pin to cpu %d\n", a.cpu);
  lock_memory();
  const TscClock clk = TscClock::calibrate(200);
  const Stream s = load(a);
  const auto locs = pick_locates(a, s);
  std::fprintf(stderr, "messages=%zu tracked_locates=%zu tsc=%.3f GHz cpu=%s\n", s.offs.size(), locs.size(),
               clk.ticks_per_ns, cpu_model().c_str());

  std::vector<Outcome> outs;
  auto want = [&](const char* v) { return a.variant == "all" || a.variant == v; };
  for (int r = 0; r < a.repeat; ++r) {
    using H = OrderIdMap::Hash;
    if (want("v0")) outs.push_back(run_variant<NaiveBook, TextBuilder, true>("v0_naive", s, locs, clk));
    if (want("v1")) outs.push_back(run_variant<MarketBook, TextBuilder, true>("v1_flat_book", s, locs, clk));
    if (want("v2")) outs.push_back(run_variant<MarketBook, TextBuilder, false>("v2_inplace_parse", s, locs, clk));
    if (want("v3")) outs.push_back(run_variant<MarketBook, PreserializedBuilder, false>("v3_preserialized", s, locs, clk));
    if (want("v4"))
      outs.push_back(run_variant<MarketBook, PreserializedBuilder, false>("v4_identity_hash", s, locs, clk, H::Identity));
    if (want("v5"))
      outs.push_back(
          run_variant<MarketBook, PreserializedBuilder, false>("v5_hugepages", s, locs, clk, H::Identity, true));
  }
  for (const auto& o : outs) {
    std::printf("%s%s%s: %.2f M msg/s, %llu orders\n", a.label.c_str(), a.label.empty() ? "" : " ", o.name.c_str(),
                static_cast<double>(o.messages) / o.seconds / 1e6, static_cast<unsigned long long>(o.orders));
    print_stages(o.h, clk);
  }
  if (!a.json.empty()) {
    std::ofstream f(a.json);
    f << "{\"bench\":\"pipeline\",\"label\":\"" << a.label << "\",\"cpu\":\"" << cpu_model()
      << "\",\"tsc_ghz\":" << clk.ticks_per_ns << ",\"source\":\"" << (a.file.empty() ? "synthetic" : "file")
      << "\",\"messages\":" << s.offs.size() << ",\"variants\":[";
    for (std::size_t i = 0; i < outs.size(); ++i) {
      const auto& o = outs[i];
      f << (i ? "," : "") << "{\"name\":\"" << o.name << "\",\"msgs_per_sec\":"
        << static_cast<double>(o.messages) / o.seconds << ",\"orders\":" << o.orders
        << ",\"stages\":" << stages_json(o.h, clk, true) << "}";
    }
    f << "]}\n";
  }
  return 0;
}
