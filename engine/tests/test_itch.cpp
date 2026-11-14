#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "lle/itch.hpp"
#include "lle/itch_writer.hpp"

using namespace lle;
using namespace lle::itch;

namespace {
struct Rec : NullHandler {
  std::vector<std::string> log;
  void on_add(const AddOrder& m) {
    log.push_back("A " + std::to_string(m.locate()) + " " + std::to_string(m.ref()) + " " +
                  (m.side() == Side::Buy ? "B" : "S") + " " + std::to_string(m.shares()) + " " +
                  std::string(m.stock(), 8) + " " + std::to_string(m.price()) + " " +
                  std::to_string(m.timestamp_ns()));
  }
  void on_executed(const OrderExecuted& m) { log.push_back("E " + std::to_string(m.ref()) + " " + std::to_string(m.shares())); }
  void on_cancel(const OrderCancel& m) { log.push_back("X " + std::to_string(m.ref()) + " " + std::to_string(m.shares())); }
  void on_delete(const OrderDelete& m) { log.push_back("D " + std::to_string(m.ref())); }
  void on_replace(const OrderReplace& m) {
    log.push_back("U " + std::to_string(m.old_ref()) + " " + std::to_string(m.new_ref()) + " " +
                  std::to_string(m.shares()) + " " + std::to_string(m.price()));
  }
  void on_system(const SystemEvent& m) { log.push_back(std::string("S ") + m.event_code()); }
};
}  // namespace

TEST(Itch, RoundTripThroughWriter) {
  Writer w;
  w.system_event(1, 'O');
  w.add(7, 123456789012ull, 42, Side::Buy, 300, "AAPL", 1'901'200);
  w.executed(7, 2, 42, 100, 9);
  w.cancel(7, 3, 42, 50);
  w.replace(7, 4, 42, 43, 500, 1'901'300);
  w.del(7, 5, 43);
  Rec r;
  ParseStats st;
  const auto used = parse_stream(w.buf.data(), w.buf.size(), r, st);
  EXPECT_EQ(used, w.buf.size());
  ASSERT_EQ(r.log.size(), 6u);
  EXPECT_EQ(r.log[0], "S O");
  EXPECT_EQ(r.log[1], "A 7 42 B 300 AAPL     1901200 123456789012");
  EXPECT_EQ(r.log[2], "E 42 100");
  EXPECT_EQ(r.log[3], "X 42 50");
  EXPECT_EQ(r.log[4], "U 42 43 500 1901300");
  EXPECT_EQ(r.log[5], "D 43");
  EXPECT_EQ(st.messages, 6u);
  EXPECT_EQ(st.malformed, 0u);
}

// Known-answer test against bytes laid out by hand from the ITCH 5.0 spec.
TEST(Itch, AddOrderKnownBytes) {
  const unsigned char m[36] = {'A', 0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,  // ts=258
                               0, 0, 0, 0, 0, 0, 0x01, 0x00,                                  // ref=256
                               'S', 0x00, 0x00, 0x00, 0x64,                                  // 100 shares
                               'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',                        // stock
                               0x00, 0x05, 0xF5, 0xE1};                                      // 390625
  Rec r;
  ParseStats st;
  EXPECT_TRUE(dispatch(m, sizeof(m), r, st));
  ASSERT_EQ(r.log.size(), 1u);
  EXPECT_EQ(r.log[0], "A 13 256 S 100 MSFT     390625 258");
}

TEST(Itch, TruncatedMessagesRejected) {
  Writer w;
  w.add(1, 1, 1, Side::Sell, 1, "X", 100);
  Rec r;
  ParseStats st;
  // Message body without its last byte.
  EXPECT_FALSE(dispatch(w.buf.data() + 2, 35, r, st));
  EXPECT_EQ(st.malformed, 1u);
  EXPECT_TRUE(r.log.empty());
  // Partial stream: nothing consumed.
  EXPECT_EQ(parse_stream(w.buf.data(), w.buf.size() - 1, r, st), 0u);
}

TEST(Itch, UnknownTypeCounted) {
  const unsigned char m[5] = {'z', 0, 0, 0, 0};
  Rec r;
  ParseStats st;
  EXPECT_FALSE(dispatch(m, sizeof(m), r, st));
  EXPECT_EQ(st.unknown, 1u);
}

TEST(Itch, MoldPacketsCarryAllMessages) {
  SynthConfig cfg;
  cfg.messages = 5000;
  const auto day = synth_day(cfg);
  const auto pkts = to_mold_packets(day, 1400, 8);
  Rec a, b;
  ParseStats sa, sb;
  parse_stream(day.data(), day.size(), a, sa);
  std::uint64_t expect_seq = 1;
  for (const auto& p : pkts) {
    EXPECT_EQ(MoldHeader{p.data()}.seq(), expect_seq);
    expect_seq += MoldHeader{p.data()}.count();
    parse_mold(p.data(), p.size(), b, sb);
  }
  EXPECT_EQ(a.log, b.log);
  EXPECT_EQ(sa.messages, sb.messages);
  EXPECT_EQ(sb.malformed, 0u);
}

TEST(Itch, MoldRejectsLyingCount) {
  std::vector<unsigned char> p(itch::kMoldHeader, 0);
  store_be<std::uint16_t>(p.data() + 18, 3);  // claims 3 messages, has 0
  Rec r;
  ParseStats st;
  EXPECT_EQ(parse_mold(p.data(), p.size(), r, st), 0);
  EXPECT_EQ(st.malformed, 1u);
}
