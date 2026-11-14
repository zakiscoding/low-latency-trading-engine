#pragma once
// UDP sockets with kernel receive timestamps (SO_TIMESTAMPING) and three receive
// strategies, so each can be measured:
//   Blocking  recvmsg() sleeps in the kernel until a packet arrives
//   Spin      non-blocking recvmsg() in a tight loop (+ SO_BUSY_POLL if allowed)
//   Uring     io_uring IORING_OP_RECVMSG, completions polled from user space
// Send is a plain send() on a connected socket. TX software timestamps go to the
// socket error queue and are drained only when the loop is idle.
#include <arpa/inet.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef LLE_HAVE_URING
#include <liburing.h>
#endif

#include "lle/common.hpp"

namespace lle::net {

enum class RxMode { Blocking, Spin, Uring };

inline RxMode parse_mode(const std::string& s) {
  if (s == "blocking") return RxMode::Blocking;
  if (s == "uring") return RxMode::Uring;
  return RxMode::Spin;
}

inline sockaddr_in make_addr(const std::string& host, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) throw std::runtime_error("bad IPv4 address: " + host);
  return a;
}

// "host:port" -> sockaddr_in
inline sockaddr_in parse_hostport(const std::string& hp) {
  const auto c = hp.rfind(':');
  if (c == std::string::npos) throw std::runtime_error("expected host:port, got " + hp);
  return make_addr(hp.substr(0, c), std::stoi(hp.substr(c + 1)));
}

inline int udp_socket() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
  return fd;
}

inline void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

// SO_RCVBUFFORCE (CAP_NET_ADMIN) ignores net.core.rmem_max, so no global sysctl change is needed.
inline void set_rcvbuf(int fd, int bytes) {
#if defined(__linux__)
  if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof(bytes)) == 0) return;
#endif
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

// Enables software RX (and optionally TX) timestamps. Returns false if refused.
// Linux only; elsewhere (macOS) this returns false and latency is measured from
// user space only.
inline bool enable_timestamps(int fd, bool tx) {
#if !defined(__linux__)
  (void)fd;
  (void)tx;
  return false;
#else
  int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  if (tx) flags |= SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_OPT_TSONLY | SOF_TIMESTAMPING_OPT_ID;
  return ::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) == 0;
#endif
}

// SO_BUSY_POLL makes a blocking/non-blocking receive poll the NIC queue for up to
// `usec` before sleeping. Needs CAP_NET_ADMIN to raise above the sysctl default.
inline bool enable_busy_poll(int fd, int usec) {
#if defined(__linux__)
  return ::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec)) == 0;
#else
  (void)fd;
  (void)usec;
  return false;
#endif
}

inline int bind_udp(const std::string& host, int port) {
  const int fd = udp_socket();
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = make_addr(host, port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
    throw std::runtime_error("bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno));
  return fd;
}

inline void connect_udp(int fd, const sockaddr_in& to) {
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) < 0)
    throw std::runtime_error("connect: " + std::string(std::strerror(errno)));
}

// Extracts the software timestamp (CLOCK_REALTIME ns) from a received msghdr.
inline std::int64_t rx_timestamp_ns(msghdr* m) noexcept {
#if !defined(__linux__)
  (void)m;
  return 0;
#else
  for (cmsghdr* c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
      timespec ts[3];
      std::memcpy(ts, CMSG_DATA(c), sizeof(ts));
      return static_cast<std::int64_t>(ts[0].tv_sec) * 1'000'000'000 + ts[0].tv_nsec;
    }
  }
  return 0;
#endif
}

// One receive slot: buffer, iovec, msghdr and control space, all preallocated.
struct alignas(kCacheLine) RxSlot {
  static constexpr std::size_t kBuf = 2048;
  alignas(kCacheLine) unsigned char buf[kBuf];
  alignas(8) unsigned char ctrl[256];
  iovec iov{};
  msghdr msg{};
  sockaddr_in from{};
  void reset() noexcept {
    iov.iov_base = buf;
    iov.iov_len = kBuf;
    msg.msg_name = &from;
    msg.msg_namelen = sizeof(from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    msg.msg_flags = 0;
  }
};

struct Packet {
  const unsigned char* data;
  std::size_t len;
  std::int64_t kernel_rx_ns;  // 0 if the kernel gave no timestamp
};

// Receiver over one socket. poll() returns true with a packet, or false if none
// arrived (Spin/Uring) or on EINTR (Blocking). The returned payload stays valid
// until the next poll().
class Receiver {
 public:
  Receiver(int fd, RxMode mode) : fd_(fd), mode_(mode) {
    s_[0].reset();
    s_[1].reset();
    if (mode_ == RxMode::Spin) set_nonblocking(fd_);
#ifdef LLE_HAVE_URING
    if (mode_ == RxMode::Uring) {
      if (io_uring_queue_init(8, &ring_, 0) < 0) throw std::runtime_error("io_uring_queue_init failed (seccomp?)");
      uring_ok_ = true;
      arm();
    }
#else
    if (mode_ == RxMode::Uring) throw std::runtime_error("built without liburing");
#endif
  }
  ~Receiver() {
#ifdef LLE_HAVE_URING
    if (uring_ok_) io_uring_queue_exit(&ring_);
#endif
  }
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  LLE_ALWAYS_INLINE bool poll(Packet& p) noexcept {
    if (mode_ != RxMode::Uring) {
      RxSlot& s = s_[0];
      s.msg.msg_controllen = sizeof(s.ctrl);
      const ssize_t n = ::recvmsg(fd_, &s.msg, 0);
      if (n <= 0) return false;
      p = Packet{s.buf, static_cast<std::size_t>(n), rx_timestamp_ns(&s.msg)};
      return true;
    }
#ifdef LLE_HAVE_URING
    io_uring_cqe* cqe = nullptr;
    if (io_uring_peek_cqe(&ring_, &cqe) != 0 || !cqe) return false;
    const int res = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    RxSlot& done = s_[idx_];
    idx_ ^= 1;  // next receive goes into the other slot, so `done` stays readable
    arm();
    if (res > 0) {
      p = Packet{done.buf, static_cast<std::size_t>(res), rx_timestamp_ns(&done.msg)};
      return true;
    }
#endif
    return false;
  }

  int fd() const noexcept { return fd_; }

 private:
#ifdef LLE_HAVE_URING
  void arm() noexcept {
    RxSlot& s = s_[idx_];
    s.reset();
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_recvmsg(sqe, fd_, &s.msg, 0);
    io_uring_submit(&ring_);
  }
  io_uring ring_{};
  bool uring_ok_ = false;
#endif
  int fd_;
  RxMode mode_;
  int idx_ = 0;
  RxSlot s_[2];
};

// Connected UDP sender. send() is the only syscall on the hot path.
class Sender {
 public:
  explicit Sender(const sockaddr_in& to, bool tx_timestamps = false) : fd_(udp_socket()) {
    connect_udp(fd_, to);
    if (tx_timestamps) tx_ts_ = enable_timestamps(fd_, true);
  }
  ~Sender() { ::close(fd_); }
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  LLE_ALWAYS_INLINE bool send(const unsigned char* p, std::size_t n) noexcept {
    return ::send(fd_, p, n, MSG_DONTWAIT) == static_cast<ssize_t>(n);
  }

  // Drains TX timestamps (idle path). f(id, realtime_ns) for each one.
  template <class F>
  int drain_tx_timestamps(F&& f) noexcept {
    if (!tx_ts_) return 0;
#if !defined(__linux__)
    (void)f;
    return 0;
#else
    int k = 0;
    unsigned char ctrl[512];
    unsigned char data[64];
    for (;;) {
      iovec iov{data, sizeof(data)};
      msghdr m{};
      m.msg_iov = &iov;
      m.msg_iovlen = 1;
      m.msg_control = ctrl;
      m.msg_controllen = sizeof(ctrl);
      if (::recvmsg(fd_, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) break;
      std::int64_t ts = 0;
      std::uint32_t id = 0;
      for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
          timespec t[3];
          std::memcpy(t, CMSG_DATA(c), sizeof(t));
          ts = static_cast<std::int64_t>(t[0].tv_sec) * 1'000'000'000 + t[0].tv_nsec;
        } else if ((c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
                   (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR)) {
          sock_extended_err se;
          std::memcpy(&se, CMSG_DATA(c), sizeof(se));
          id = se.ee_data;
        }
      }
      if (ts) f(id, ts);
      k++;
    }
    return k;
#endif
  }

  int fd() const noexcept { return fd_; }
  bool tx_timestamps() const noexcept { return tx_ts_; }

 private:
  int fd_;
  bool tx_ts_ = false;
};

}  // namespace lle::net
