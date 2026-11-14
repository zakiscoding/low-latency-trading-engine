#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lle/ws.hpp"

using namespace lle::ws;

namespace {
std::vector<unsigned char> frame(Opcode op, const std::string& payload, bool fin = true) {
  std::vector<unsigned char> f;
  f.push_back(static_cast<unsigned char>((fin ? 0x80 : 0) | op));
  const std::size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<unsigned char>(n));
  } else if (n < 65536) {
    f.push_back(126);
    f.push_back(static_cast<unsigned char>(n >> 8));
    f.push_back(static_cast<unsigned char>(n));
  } else {
    f.push_back(127);
    for (int i = 7; i >= 0; --i) f.push_back(static_cast<unsigned char>(static_cast<std::uint64_t>(n) >> (8 * i)));
  }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}
void append(std::vector<unsigned char>& a, const std::vector<unsigned char>& b) { a.insert(a.end(), b.begin(), b.end()); }

// Feeds `stream` to a parser in chunks of `chunk` bytes, simulating the client's
// buffer handling (consume from the front), and returns the events.
std::vector<std::pair<Opcode, std::string>> drive(const std::vector<unsigned char>& stream, std::size_t chunk) {
  WsParser p;
  std::vector<unsigned char> buf;
  std::size_t fed = 0;
  std::vector<std::pair<Opcode, std::string>> out;
  while (true) {
    std::size_t used;
    FrameEvent ev;
    const int r = p.next(buf.data(), buf.size(), used, ev);
    if (r < 0) { out.push_back({kClose, "ERROR"}); return out; }
    if (r == 1) {
      out.push_back({ev.op, std::string(reinterpret_cast<const char*>(ev.data), ev.len)});
      buf.erase(buf.begin(), buf.begin() + static_cast<long>(used));
      continue;
    }
    if (fed >= stream.size()) return out;
    const std::size_t k = std::min(chunk, stream.size() - fed);
    buf.insert(buf.end(), stream.begin() + static_cast<long>(fed), stream.begin() + static_cast<long>(fed + k));
    fed += k;
  }
}
}  // namespace

TEST(WsParser, LengthEncodings) {
  std::vector<unsigned char> s;
  const std::string a(10, 'a'), b(300, 'b'), c(70000, 'c');
  append(s, frame(kText, a));
  append(s, frame(kText, b));
  append(s, frame(kBinary, c));
  for (std::size_t chunk : {1ul, 7ul, 1000ul, 1ul << 20}) {
    auto ev = drive(s, chunk);
    ASSERT_EQ(ev.size(), 3u) << chunk;
    EXPECT_EQ(ev[0].second, a);
    EXPECT_EQ(ev[1].second, b);
    EXPECT_EQ(ev[2].first, kBinary);
    EXPECT_EQ(ev[2].second.size(), c.size());
  }
}

TEST(WsParser, FragmentsWithInterleavedPing) {
  std::vector<unsigned char> s;
  append(s, frame(kText, "hello ", false));
  append(s, frame(kPing, "p1"));
  append(s, frame(kCont, "big ", false));
  append(s, frame(kCont, "world"));
  append(s, frame(kText, "next"));
  for (std::size_t chunk : {1ul, 3ul, 5ul, 64ul}) {
    auto ev = drive(s, chunk);
    ASSERT_EQ(ev.size(), 3u) << chunk;
    EXPECT_EQ(ev[0].first, kPing);
    EXPECT_EQ(ev[0].second, "p1");
    EXPECT_EQ(ev[1].second, "hello big world");
    EXPECT_EQ(ev[2].second, "next");
  }
}

TEST(WsParser, ProtocolErrors) {
  auto err = [](std::vector<unsigned char> f) {
    auto ev = drive(f, 1 << 20);
    return !ev.empty() && ev.back().second == "ERROR";
  };
  auto masked = frame(kText, "x");
  masked[1] |= 0x80;
  EXPECT_TRUE(err(masked));
  EXPECT_TRUE(err(frame(kCont, "orphan")));
  auto rsv = frame(kText, "x");
  rsv[0] |= 0x40;
  EXPECT_TRUE(err(rsv));
  EXPECT_TRUE(err(frame(kPing, std::string(126, 'p'))));
  std::vector<unsigned char> two;
  append(two, frame(kText, "a", false));
  append(two, frame(kText, "b"));  // new data frame inside a fragmented message
  EXPECT_TRUE(err(two));
  std::vector<unsigned char> huge = {0x81, 127, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_TRUE(err(huge));
}

TEST(WsParser, ClientFramesAreMasked) {
  std::vector<unsigned char> out;
  mask_frame(out, kText, "subscribe");
  ASSERT_EQ(out.size(), 2u + 4u + 9u);
  EXPECT_EQ(out[0], 0x81);
  EXPECT_EQ(out[1], 0x80 | 9);
  std::string un;
  for (std::size_t i = 0; i < 9; ++i) un.push_back(static_cast<char>(out[6 + i] ^ out[2 + (i & 3)]));
  EXPECT_EQ(un, "subscribe");
}
