#include <gtest/gtest.h>

#include "lle/xdp.hpp"

#if defined(__linux__)
#include <sys/time.h>

using namespace lle;

// Unprivileged: the hand-assembled program is well formed.
TEST(Xdp, ProgramShape) {
  const auto p = xdp::build_program(-1, 31001);
  ASSERT_GT(p.size(), 10u);
  EXPECT_EQ(p.back().code, BPF_JMP | BPF_EXIT);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto cls = p[i].code & 0x07;
    if (cls == BPF_JMP && (p[i].code & 0xf0) != BPF_CALL && (p[i].code & 0xf0) != BPF_EXIT) {
      const long t = static_cast<long>(i) + 1 + p[i].off;
      EXPECT_GT(t, static_cast<long>(i)) << "backward jump at " << i;
      EXPECT_LT(t, static_cast<long>(p.size())) << "jump out of range at " << i;
    }
  }
  // The port compare uses the network-order value as a little-endian load sees it.
  bool found = false;
  for (const auto& i : p)
    if (i.code == (BPF_JMP | BPF_JNE | BPF_K) && i.imm == __builtin_bswap16(31001)) found = true;
  EXPECT_TRUE(found);
}

// Privileged (CAP_NET_ADMIN + CAP_BPF, e.g. a --privileged container): attach to
// the loopback device in generic mode, send UDP to the filtered port and to a
// different port, and check only the first arrives through AF_XDP, with a sane
// XDP-hook timestamp. Skipped elsewhere (normal CI runners).
TEST(Xdp, LoopbackEndToEnd) {
  constexpr int kPort = 39123;
  std::unique_ptr<xdp::XdpReceiver> rx;
  try {
    xdp::Config c;
    c.ifname = "lo";
    c.udp_port = kPort;
    c.frames = 256;
    rx = std::make_unique<xdp::XdpReceiver>(c);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "AF_XDP not available here: " << e.what();
  }
  // Bound sockets on both ports so the kernel doesn't answer with ICMP.
  const int other = net::bind_udp("127.0.0.1", kPort + 1);
  net::set_nonblocking(other);
  net::Sender to_xdp(net::make_addr("127.0.0.1", kPort));
  net::Sender to_other(net::make_addr("127.0.0.1", kPort + 1));

  const unsigned char a[] = "hello-xdp";
  const unsigned char b[] = "for-the-stack";
  timespec t0{};
  ::clock_gettime(CLOCK_REALTIME, &t0);
  const std::int64_t before = static_cast<std::int64_t>(t0.tv_sec) * 1'000'000'000 + t0.tv_nsec;
  ASSERT_TRUE(to_other.send(b, sizeof(b) - 1));
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(to_xdp.send(a, sizeof(a) - 1));

  int got = 0;
  for (int spins = 0; spins < 50'000'000 && got < 3; ++spins) {
    net::Packet p;
    if (!rx->poll(p)) continue;
    ASSERT_EQ(p.len, sizeof(a) - 1);
    EXPECT_EQ(std::memcmp(p.data, a, p.len), 0);
    // The stamp is CLOCK_MONOTONIC moved to REALTIME; within a second of "now".
    EXPECT_GT(p.kernel_rx_ns, before - 1'000'000'000);
    EXPECT_LT(p.kernel_rx_ns, before + 1'000'000'000);
    got++;
  }
  EXPECT_EQ(got, 3);
  unsigned char buf[64];
  ssize_t n = -1;
  for (int i = 0; i < 1000 && n < 0; ++i) n = ::recv(other, buf, sizeof(buf), 0);
  EXPECT_EQ(n, static_cast<ssize_t>(sizeof(b) - 1)) << "non-matching port must reach the normal stack";
  ::close(other);
}

#endif
