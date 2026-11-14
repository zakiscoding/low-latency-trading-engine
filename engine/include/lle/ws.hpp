#pragma once
// Minimal WebSocket-over-TLS client for public market-data feeds.
//
//  - TCP bytes are read with our own recvmsg() (so SO_TIMESTAMPING still gives a
//    kernel RX stamp), pushed into an OpenSSL memory BIO, and decrypted with
//    SSL_read into one large preallocated plaintext buffer.
//  - Frames are parsed in place in that buffer; a complete text message is handed
//    to the callback as (pointer, length) with no copy. Fragmented messages are
//    reassembled by moving payloads down over the frame headers (rare).
//  - Client frames (pong, subscribe) are masked as RFC 6455 requires.
// The parser part (WsParser) has no I/O and is what the fuzzer drives.
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include "lle/tsc.hpp"
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lle/common.hpp"
#include "lle/memory.hpp"
#include "lle/net.hpp"
#include "lle/safety.hpp"

namespace lle::ws {

enum Opcode : std::uint8_t { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

struct FrameEvent {
  Opcode op;
  const unsigned char* data;
  std::size_t len;
};

// Incremental frame parser over a caller-owned buffer [buf, buf+n), where buf is
// always the start of the not-yet-consumed bytes.
// next() returns 1 = event ready in ev, 0 = need more bytes, -1 = protocol error.
// `consumed` is how many bytes at buf the caller may now drop. While a fragmented
// message is being assembled nothing is consumed: continuation payloads are moved
// down to sit right after the first fragment's payload, and the scan position is
// remembered as an offset from buf (so the caller may move the whole region).
class WsParser {
 public:
  static constexpr std::size_t kMaxMessage = 64u << 20;

  int next(unsigned char* buf, std::size_t n, std::size_t& consumed, FrameEvent& ev) noexcept {
    consumed = 0;
    std::size_t off = in_frag_ ? scan_ : 0;
    for (;;) {
      if (n < off + 2) return pause(off);
      const unsigned char b0 = buf[off], b1 = buf[off + 1];
      const bool fin = b0 & 0x80;
      const auto op = static_cast<Opcode>(b0 & 0x0F);
      if (b0 & 0x70) return -1;  // RSV bits: no extensions negotiated
      if (b1 & 0x80) return -1;  // server frames must not be masked
      std::uint64_t len = b1 & 0x7F;
      std::size_t hdr = 2;
      if (len == 126) {
        if (n < off + 4) return pause(off);
        len = load_be<std::uint16_t>(buf + off + 2);
        hdr = 4;
      } else if (len == 127) {
        if (n < off + 10) return pause(off);
        len = load_be<std::uint64_t>(buf + off + 2);
        hdr = 10;
      }
      if (len > kMaxMessage) return -1;
      if (n - off - hdr < len) return pause(off);
      const std::size_t pay = off + hdr;
      if (op & 0x8) {  // control frame (may be interleaved inside a fragmented message)
        if (!fin || len > 125) return -1;
        if (op != kClose && op != kPing && op != kPong) return -1;
        ev = {op, buf + pay, static_cast<std::size_t>(len)};
        if (in_frag_) scan_ = pay + len; else consumed = pay + len;
        return 1;
      }
      if (op == kCont) {
        if (!in_frag_) return -1;
        std::memmove(buf + start_ + frag_len_, buf + pay, len);
        frag_len_ += len;
      } else {
        if (in_frag_) return -1;
        if (op != kText && op != kBinary) return -1;
        frag_op_ = op;
        start_ = pay;
        frag_len_ = len;
      }
      if (frag_len_ > kMaxMessage) return -1;
      off = pay + len;
      if (fin) {
        in_frag_ = false;
        consumed = off;
        ev = {frag_op_, buf + start_, static_cast<std::size_t>(frag_len_)};
        return 1;
      }
      in_frag_ = true;
      scan_ = off;
    }
  }
  void reset() noexcept { in_frag_ = false; }

 private:
  int pause(std::size_t off) noexcept {
    if (in_frag_) scan_ = off;
    return 0;
  }
  bool in_frag_ = false;
  Opcode frag_op_ = kText;
  std::size_t start_ = 0, scan_ = 0;
  std::uint64_t frag_len_ = 0;
};

inline void mask_frame(std::vector<unsigned char>& out, Opcode op, std::string_view payload) {
  out.clear();
  out.push_back(static_cast<unsigned char>(0x80 | op));
  const std::size_t n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<unsigned char>(0x80 | n));
  } else if (n < 65536) {
    out.push_back(0x80 | 126);
    out.push_back(static_cast<unsigned char>(n >> 8));
    out.push_back(static_cast<unsigned char>(n));
  } else {
    out.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<unsigned char>(n >> (8 * i)));
  }
  unsigned char key[4];
  RAND_bytes(key, 4);
  out.insert(out.end(), key, key + 4);
  for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<unsigned char>(payload[i]) ^ key[i & 3]);
}

// TLS WebSocket client. connect() blocks (startup only); poll() is non-blocking.
class Client {
 public:
  Client() {
    plain_cap_ = 96u << 20;
    plain_ = static_cast<unsigned char*>(table_alloc(plain_cap_));
  }
  ~Client() {
    if (ssl_) SSL_free(ssl_);
    if (ctx_) SSL_CTX_free(ctx_);
    if (fd_ >= 0) ::close(fd_);
    table_free(plain_, plain_cap_);
  }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void connect(const std::string& host, const std::string& path, int port = 443) {
    safety::require_allowed("wss://" + host);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
      throw std::runtime_error("resolve " + host);
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
      freeaddrinfo(res);
      throw std::runtime_error("connect " + host);
    }
    freeaddrinfo(res);
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    rx_ts_ = net::enable_timestamps(fd_, false);

    ctx_ = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(ctx_);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    ssl_ = SSL_new(ctx_);
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    SSL_set1_host(ssl_, host.c_str());
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(rbio_, -1);
    SSL_set_bio(ssl_, rbio_, wbio_);
    SSL_set_connect_state(ssl_);
    // Handshake with blocking socket I/O through the memory BIOs.
    while (true) {
      const int r = SSL_do_handshake(ssl_);
      flush_wbio();
      if (r == 1) break;
      const int e = SSL_get_error(ssl_, r);
      if (e != SSL_ERROR_WANT_READ) throw std::runtime_error("TLS handshake failed: " + ssl_err());
      if (!read_socket_blocking()) throw std::runtime_error("TLS: connection closed during handshake");
    }
    // HTTP upgrade.
    unsigned char k[16];
    RAND_bytes(k, 16);
    const std::string key = b64(k, 16);
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                            "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: lle-feed/0.1\r\n\r\n";
    write_plain(req.data(), req.size());
    while (true) {
      drain_ssl();
      const std::string_view v(reinterpret_cast<char*>(plain_), plain_len_);
      const auto end = v.find("\r\n\r\n");
      if (end != std::string_view::npos) {
        if (v.substr(0, 12).find("101") == std::string_view::npos)
          throw std::runtime_error("WebSocket upgrade refused: " + std::string(v.substr(0, std::min<std::size_t>(v.size(), 80))));
        consume(end + 4);
        break;
      }
      if (!read_socket_blocking()) throw std::runtime_error("closed during upgrade");
    }
    net::set_nonblocking(fd_);
  }

  void send_text(std::string_view s) {
    mask_frame(tmp_, kText, s);
    write_plain(tmp_.data(), tmp_.size());
  }

  // Reads what is available and calls f(data, len, kernel_rx_ns) for each complete
  // text/binary message. Returns false if the connection is gone.
  template <class F>
  bool poll(F&& f) {
    bool got = false;
    for (;;) {
      unsigned char raw[65536];
      iovec iov{raw, sizeof(raw)};
      msghdr m{};
      m.msg_iov = &iov;
      m.msg_iovlen = 1;
      m.msg_control = ctrl_;
      m.msg_controllen = sizeof(ctrl_);
      const ssize_t n = ::recvmsg(fd_, &m, 0);
      if (n == 0) return false;
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
      }
      if (rx_ts_) {
        const std::int64_t t = net::rx_timestamp_ns(&m);
        if (t) last_rx_ns_ = t;
      }
      BIO_write(rbio_, raw, static_cast<int>(n));
      got = true;
    }
    if (got) drain_ssl();
    decrypted_tsc_ = rdtsc_unordered();
    for (;;) {
      std::size_t used = 0;
      FrameEvent ev;
      const int r = parser_.next(plain_ + parse_off_, plain_len_ - parse_off_, used, ev);
      if (r < 0) return false;
      if (r == 0) break;
      parse_off_ += used;
      if (ev.op == kPing) {
        mask_frame(tmp_, kPong, std::string_view(reinterpret_cast<const char*>(ev.data), ev.len));
        write_plain(tmp_.data(), tmp_.size());
      } else if (ev.op == kClose) {
        return false;
      } else if (ev.op == kText || ev.op == kBinary) {
        messages_++;
        f(ev.data, ev.len, last_rx_ns_);
      }
    }
    // Compact: keep any partial frame at the start of the buffer.
    if (parse_off_ == plain_len_) {
      parse_off_ = plain_len_ = 0;
    } else if (parse_off_ > plain_cap_ / 2) {
      std::memmove(plain_, plain_ + parse_off_, plain_len_ - parse_off_);
      plain_len_ -= parse_off_;
      parse_off_ = 0;
    }
    return true;
  }

  int fd() const { return fd_; }
  std::uint64_t messages() const { return messages_; }
  bool rx_timestamps() const { return rx_ts_; }
  // TSC right after this poll's TLS decrypt finished (start of message handling).
  std::uint64_t decrypted_tsc() const { return decrypted_tsc_; }

 private:
  static std::string ssl_err() {
    char b[256];
    ERR_error_string_n(ERR_get_error(), b, sizeof(b));
    return b;
  }
  static std::string b64(const unsigned char* d, int n) {
    std::string out(4 * ((n + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), d, n);
    return out;
  }
  void flush_wbio() {
    char buf[16384];
    int n;
    while ((n = BIO_read(wbio_, buf, sizeof(buf))) > 0) {
      int off = 0;
      while (off < n) {
        const ssize_t w = ::send(fd_, buf + off, static_cast<std::size_t>(n - off), MSG_NOSIGNAL);
        if (w < 0) {
          if (errno == EAGAIN) { pollfd p{fd_, POLLOUT, 0}; ::poll(&p, 1, 100); continue; }
          throw std::runtime_error("send failed");
        }
        off += static_cast<int>(w);
      }
    }
  }
  bool read_socket_blocking() {
    unsigned char raw[16384];
    const ssize_t n = ::recv(fd_, raw, sizeof(raw), 0);
    if (n <= 0) return false;
    BIO_write(rbio_, raw, static_cast<int>(n));
    return true;
  }
  void write_plain(const void* p, std::size_t n) {
    SSL_write(ssl_, p, static_cast<int>(n));
    flush_wbio();
  }
  void drain_ssl() {
    for (;;) {
      if (plain_len_ == plain_cap_) throw std::runtime_error("plaintext buffer full");
      const int r = SSL_read(ssl_, plain_ + plain_len_, static_cast<int>(plain_cap_ - plain_len_));
      if (r > 0) { plain_len_ += static_cast<std::size_t>(r); continue; }
      flush_wbio();  // TLS 1.3 post-handshake messages
      break;
    }
  }
  void consume(std::size_t n) {
    std::memmove(plain_, plain_ + n, plain_len_ - n);
    plain_len_ -= n;
  }

  int fd_ = -1;
  SSL_CTX* ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  BIO* rbio_ = nullptr;
  BIO* wbio_ = nullptr;
  unsigned char* plain_ = nullptr;
  std::size_t plain_cap_ = 0, plain_len_ = 0, parse_off_ = 0;
  WsParser parser_;
  std::vector<unsigned char> tmp_;
  alignas(8) unsigned char ctrl_[256];
  bool rx_ts_ = false;
  std::int64_t last_rx_ns_ = 0;
  std::uint64_t messages_ = 0;
  std::uint64_t decrypted_tsc_ = 0;
};

}  // namespace lle::ws
