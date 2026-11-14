#include <gtest/gtest.h>

#include <random>

#include "lle/iex.hpp"

using namespace lle;
using namespace lle::iex;

namespace {

struct Collect {
  std::vector<PriceLevel> levels;
  std::vector<Quote> quotes;
  std::vector<Trade> trades;
  void on_level(const PriceLevel& u) { levels.push_back(u); }
  void on_quote(const Quote& q) { quotes.push_back(q); }
  void on_trade(const Trade& t) { trades.push_back(t); }
};

std::vector<unsigned char> sample_capture() {
  std::vector<unsigned char> cap;
  pcap_header(cap);
  SegmentWriter w(kProtoDeep);
  w.level(true, false, 1, "AAPL", 100, 1'850'100);   // 185.01
  w.level(true, true, 2, "AAPL", 200, 1'850'000);    // 185.00, batch complete
  w.level(false, true, 3, "AAPL", 300, 1'850'500);   // 185.05
  pcap_udp(cap, 1'000, w.flush(10));
  w.trade(4, "AAPL", 50, 1'850'500, 77);
  w.level(false, true, 5, "AAPL", 0, 1'850'500);     // ask level removed
  w.level(false, true, 6, "AAPL", 10, 1'851'000);
  w.raw('S', 10);                                    // system event: counted, ignored
  pcap_udp(cap, 2'000, w.flush(20));
  return cap;
}

}  // namespace

TEST(Iex, SymbolKeyRoundTrip) {
  EXPECT_EQ(symbol_str(symbol_key("AAPL")), "AAPL");
  EXPECT_EQ(symbol_str(symbol_key("BRK.A")), "BRK.A");
  EXPECT_NE(symbol_key("AAPL"), symbol_key("AAPLX"));
}

TEST(Iex, PcapToBooks) {
  const auto cap = sample_capture();
  CaptureReader rd(cap.data(), cap.size());
  ASSERT_TRUE(rd.ok());
  L2Books books(4);
  struct H {
    L2Books& b;
    int trades = 0;
    void on_level(const PriceLevel& u) { b.apply(u); }
    void on_quote(const Quote&) {}
    void on_trade(const Trade&) { trades++; }
  } h{books};
  Stats st;
  SeqState seq;
  Frame f;
  int frames = 0;
  while (rd.next(f)) {
    const unsigned char* pl;
    std::size_t pn;
    std::uint16_t dport = 0;
    ASSERT_TRUE(udp_payload(f.data, f.len, pl, pn, &dport));
    EXPECT_EQ(dport, 10378);
    EXPECT_TRUE(parse_segment(pl, pn, h, st, &seq));
    frames++;
  }
  EXPECT_EQ(frames, 2);
  EXPECT_EQ(st.segments, 2u);
  EXPECT_EQ(st.messages, 7u);
  EXPECT_EQ(st.levels, 5u);
  EXPECT_EQ(st.trades, 1u);
  EXPECT_EQ(st.other, 1u);
  EXPECT_EQ(st.gaps, 0u);
  EXPECT_EQ(st.malformed, 0u);
  auto* b = books.find("AAPL");
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->bids.best_price(), 18501);
  EXPECT_EQ(b->bids.best_qty(), 100);
  EXPECT_EQ(b->asks.best_price(), 18510);
  EXPECT_EQ(b->asks.best_qty(), 10);
  EXPECT_EQ(b->asks.qty_at(18505), 0);
  EXPECT_TRUE(b->consistent);
}

TEST(Iex, FieldsDecoded) {
  SegmentWriter w(kProtoTops);
  w.quote(123, "MSFT", 300, 4'101'200, 4'101'900, 200);
  w.trade(124, "MSFT", 25, 4'101'500, 9);
  const auto s = w.flush();
  Collect c;
  Stats st;
  ASSERT_TRUE(parse_segment(s.data(), s.size(), c, st));
  ASSERT_EQ(c.quotes.size(), 1u);
  EXPECT_EQ(c.quotes[0].ts, 123);
  EXPECT_EQ(c.quotes[0].symbol, symbol_key("MSFT"));
  EXPECT_EQ(c.quotes[0].bid_size, 300u);
  EXPECT_EQ(c.quotes[0].bid, 4'101'200);
  EXPECT_EQ(c.quotes[0].ask, 4'101'900);
  EXPECT_EQ(c.quotes[0].ask_size, 200u);
  ASSERT_EQ(c.trades.size(), 1u);
  EXPECT_EQ(c.trades[0].size, 25u);
  EXPECT_EQ(c.trades[0].price, 4'101'500);
  EXPECT_EQ(c.trades[0].trade_id, 9);
}

TEST(Iex, SequenceGapDetected) {
  SegmentWriter w(kProtoDeep);
  w.level(true, true, 1, "A", 1, 10000);
  const auto s1 = w.flush();
  w.skip(5);
  w.level(true, true, 2, "A", 1, 10100);
  const auto s2 = w.flush();
  Collect c;
  Stats st;
  SeqState seq;
  parse_segment(s1.data(), s1.size(), c, st, &seq);
  parse_segment(s2.data(), s2.size(), c, st, &seq);
  EXPECT_EQ(st.gaps, 1u);
}

TEST(Iex, SubPennyAndUntrackedSkipped) {
  L2Books books(1);
  EXPECT_NE(books.apply(PriceLevel{true, true, 0, symbol_key("A"), 5, 12'300}), nullptr);
  EXPECT_EQ(books.apply(PriceLevel{true, true, 0, symbol_key("A"), 5, 12'345}), nullptr);  // $1.2345
  EXPECT_EQ(books.apply(PriceLevel{true, true, 0, symbol_key("B"), 5, 12'300}), nullptr);  // over max_books
  EXPECT_EQ(books.subpenny(), 1u);
  EXPECT_EQ(books.untracked(), 1u);
}

TEST(Iex, RejectsFragmentsAndNonUdp) {
  auto cap = sample_capture();
  CaptureReader rd(cap.data(), cap.size());
  Frame f;
  ASSERT_TRUE(rd.next(f));
  ASSERT_GE(f.len, 42u);
  std::vector<unsigned char> frame(f.data, f.data + f.len);
  const unsigned char* pl;
  std::size_t pn;
  auto fr = frame;
  fr[20] = 0x20;  // more-fragments flag
  EXPECT_FALSE(udp_payload(fr.data(), fr.size(), pl, pn));
  fr = frame;
  fr[23] = 6;  // TCP
  EXPECT_FALSE(udp_payload(fr.data(), fr.size(), pl, pn));
  // One 802.1Q tag is accepted.
  fr = frame;
  const unsigned char tag[4] = {0x81, 0x00, 0x00, 0x05};
  fr.insert(fr.begin() + 12, tag, tag + 4);
  EXPECT_TRUE(udp_payload(fr.data(), fr.size(), pl, pn));
}

TEST(Iex, Pcapng) {
  // SHB, IDB (ns resolution), one EPB carrying the first frame of the sample.
  const auto cap = sample_capture();
  CaptureReader rd0(cap.data(), cap.size());
  Frame f0;
  ASSERT_TRUE(rd0.next(f0));
  std::vector<unsigned char> ng;
  auto u32 = [&](std::uint32_t v) { ng.insert(ng.end(), reinterpret_cast<unsigned char*>(&v), reinterpret_cast<unsigned char*>(&v) + 4); };
  auto u16 = [&](std::uint16_t v) { ng.insert(ng.end(), reinterpret_cast<unsigned char*>(&v), reinterpret_cast<unsigned char*>(&v) + 2); };
  u32(0x0a0d0d0a); u32(28); u32(0x1a2b3c4d); u16(1); u16(0); u32(0xffffffff); u32(0xffffffff); u32(28);
  u32(1); u32(32); u16(1); u16(0); u32(65535); u16(9); u16(1); ng.push_back(9); ng.push_back(0); ng.push_back(0); ng.push_back(0);
  u16(0); u16(0); u32(32);
  const std::uint32_t padded = (static_cast<std::uint32_t>(f0.len) + 3u) & ~3u;
  const std::uint64_t ts = 5'000'000'123ull;
  u32(6); u32(32 + padded); u32(0); u32(static_cast<std::uint32_t>(ts >> 32)); u32(static_cast<std::uint32_t>(ts));
  u32(static_cast<std::uint32_t>(f0.len)); u32(static_cast<std::uint32_t>(f0.len));
  ng.insert(ng.end(), f0.data, f0.data + f0.len);
  ng.resize(ng.size() + (padded - f0.len), 0);
  u32(32 + padded);
  CaptureReader rd(ng.data(), ng.size());
  ASSERT_TRUE(rd.ok());
  Frame f;
  ASSERT_TRUE(rd.next(f));
  EXPECT_EQ(f.len, f0.len);
  EXPECT_EQ(f.ts_ns, 5'000'000'123);
  EXPECT_EQ(std::memcmp(f.data, f0.data, f.len), 0);
  EXPECT_FALSE(rd.next(f));
}

// Random truncation and corruption must never read out of bounds (ASan job).
TEST(Iex, TruncatedAndCorruptSegments) {
  SegmentWriter w(kProtoDeep);
  for (int i = 0; i < 20; ++i) w.level(i & 1, true, i, "XYZ", 10 + i, 100'000 + i * 100);
  const auto s = w.flush();
  std::mt19937 rng(3);
  Collect c;
  Stats st;
  for (std::size_t n = 0; n < s.size(); ++n) parse_segment(s.data(), n, c, st);
  for (int k = 0; k < 2000; ++k) {
    auto x = s;
    x[rng() % x.size()] = static_cast<unsigned char>(rng());
    parse_segment(x.data(), x.size(), c, st);
  }
  SUCCEED();
}
