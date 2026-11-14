#pragma once
// IEX historical data (IEX HIST): pcap / pcapng captures of the IEX-TP feed over
// UDP. Two feeds are parsed in place, no copies:
//   DEEP 1.0  price-level updates ('8' buy, '5' sell), aggregated size per price
//   TOPS 1.6  quote updates ('Q'): best bid/offer
// Both also carry trade reports ('T'). Prices are int64 with 4 implied decimals.
//
// Layers:
//   CaptureReader     pcap (us/ns) or pcapng -> Ethernet frames
//   udp_payload()     Ethernet (+VLAN) / IPv4 / UDP -> payload (no fragments)
//   parse_segment()   IEX-TP header (40 bytes, little-endian) + length-prefixed messages
//   L2Books           per-symbol PriceLadders fed by DEEP; consistent only when a
//                     message carries the "event processing complete" flag
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lle/common.hpp"
#include "lle/ladder.hpp"

namespace lle::iex {

template <class T>
LLE_ALWAYS_INLINE T load_le(const unsigned char* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;  // x86-64 and arm64 are little-endian
}

// ---------------------------------------------------------------- capture files
struct Frame {
  const unsigned char* data;
  std::size_t len;
  std::int64_t ts_ns;  // capture timestamp
};

class CaptureReader {
 public:
  CaptureReader(const unsigned char* p, std::size_t n) : p_(p), end_(p + n) {
    if (n < 24) return;
    const std::uint32_t magic = load_le<std::uint32_t>(p);
    if (magic == 0xa1b2c3d4u || magic == 0xa1b23c4du) {
      kind_ = Kind::Pcap;
      ns_ = magic == 0xa1b23c4du;
      link_ = load_le<std::uint32_t>(p + 20);
      cur_ = p + 24;
    } else if (magic == 0x0a0d0d0au) {
      kind_ = Kind::Pcapng;
      cur_ = p;
    }
  }
  bool ok() const noexcept { return kind_ != Kind::None; }
  bool ethernet() const noexcept { return link_ == 1; }

  bool next(Frame& f) noexcept {
    if (kind_ == Kind::Pcap) {
      if (end_ - cur_ < 16) return false;
      const std::uint32_t sec = load_le<std::uint32_t>(cur_);
      const std::uint32_t frac = load_le<std::uint32_t>(cur_ + 4);
      const std::uint32_t incl = load_le<std::uint32_t>(cur_ + 8);
      if (static_cast<std::size_t>(end_ - cur_ - 16) < incl) return false;
      f = Frame{cur_ + 16, incl, static_cast<std::int64_t>(sec) * 1'000'000'000 + (ns_ ? frac : frac * 1000ll)};
      cur_ += 16 + incl;
      return true;
    }
    while (kind_ == Kind::Pcapng && end_ - cur_ >= 12) {
      const std::uint32_t type = load_le<std::uint32_t>(cur_);
      const std::uint32_t blen = load_le<std::uint32_t>(cur_ + 4);
      if (blen < 12 || blen > static_cast<std::size_t>(end_ - cur_)) return false;
      const unsigned char* b = cur_;
      cur_ += blen;
      if (type == 0x0a0d0d0au) {
        if (load_le<std::uint32_t>(b + 8) != 0x1a2b3c4du) return false;  // big-endian section: unsupported
      } else if (type == 1 && blen >= 20) {                               // interface description
        link_ = load_le<std::uint16_t>(b + 8);
        tsres_ns_ = 1000;                                                 // default 10^-6
        for (const unsigned char* o = b + 16; o + 4 <= b + blen - 4;) {   // options: if_tsresol
          const std::uint16_t code = load_le<std::uint16_t>(o), olen = load_le<std::uint16_t>(o + 2);
          if (code == 0) break;
          if (code == 9 && olen >= 1) {
            const unsigned r = o[4];
            // Power-of-ten resolutions only, 10^0..10^9 s; finer ones are clamped to
            // 1 ns (a fuzzer found the unbounded loop overflowing int64).
            if (!(r & 0x80)) {
              std::int64_t div = 1;
              for (unsigned i = 0; i < r && i < 9; ++i) div *= 10;
              tsres_ns_ = 1'000'000'000 / div;
            }
          }
          o += 4 + ((olen + 3u) & ~3u);
        }
      } else if (type == 6 && blen >= 32) {  // enhanced packet
        const std::uint64_t ts = (static_cast<std::uint64_t>(load_le<std::uint32_t>(b + 12)) << 32) | load_le<std::uint32_t>(b + 16);
        const std::uint32_t cap = load_le<std::uint32_t>(b + 20);
        if (28 + static_cast<std::size_t>(cap) > blen) return false;
        f = Frame{b + 28, cap, static_cast<std::int64_t>(ts) * tsres_ns_};
        return true;
      }
    }
    return false;
  }

 private:
  enum class Kind { None, Pcap, Pcapng };
  const unsigned char* p_;
  const unsigned char* end_;
  const unsigned char* cur_ = nullptr;
  Kind kind_ = Kind::None;
  bool ns_ = false;
  std::uint32_t link_ = 1;
  std::int64_t tsres_ns_ = 1000;
};

// Ethernet (optionally one 802.1Q tag) / IPv4 / UDP. False for anything else,
// including IP fragments.
LLE_ALWAYS_INLINE bool udp_payload(const unsigned char* f, std::size_t n, const unsigned char*& pl, std::size_t& pn,
                                   std::uint16_t* dport = nullptr) noexcept {
  std::size_t o = 12;
  if (n < 14) return false;
  std::uint16_t et = load_be<std::uint16_t>(f + o);
  if (et == 0x8100) {
    o += 4;
    if (n < o + 2) return false;
    et = load_be<std::uint16_t>(f + o);
  }
  o += 2;
  if (et != 0x0800 || n < o + 20) return false;
  const unsigned ihl = (f[o] & 0x0f) * 4u;
  if ((f[o] >> 4) != 4 || ihl < 20 || f[o + 9] != 17) return false;
  if (load_be<std::uint16_t>(f + o + 6) & 0x3fff) return false;  // MF flag or fragment offset
  const std::size_t u = o + ihl;
  if (n < u + 8) return false;
  const std::size_t ulen = load_be<std::uint16_t>(f + u + 4);
  if (ulen < 8 || u + ulen > n) return false;
  if (dport) *dport = load_be<std::uint16_t>(f + u + 2);
  pl = f + u + 8;
  pn = ulen - 8;
  return true;
}

// ---------------------------------------------------------------- IEX-TP
inline constexpr std::size_t kTpHeader = 40;
inline constexpr std::uint16_t kProtoTops = 0x8003;
inline constexpr std::uint16_t kProtoDeep = 0x8004;

struct TpHeader {
  std::uint16_t protocol;
  std::uint32_t channel;
  std::uint32_t session;
  std::uint16_t payload_len;
  std::uint16_t msg_count;
  std::int64_t stream_offset;
  std::int64_t first_seq;
  std::int64_t send_time;
};

struct PriceLevel {   // DEEP '8' / '5'
  bool bid;
  bool complete;      // event flags bit 0: book is consistent after this message
  std::int64_t ts;
  std::uint64_t symbol;  // 8 ASCII bytes, space padded, as a little-endian integer
  std::uint32_t size;
  std::int64_t price;    // 1e-4 dollars
};

struct Quote {        // TOPS 'Q'
  std::int64_t ts;
  std::uint64_t symbol;
  std::uint32_t bid_size, ask_size;
  std::int64_t bid, ask;
};

struct Trade {        // 'T' (both feeds)
  std::int64_t ts;
  std::uint64_t symbol;
  std::uint32_t size;
  std::int64_t price;
  std::int64_t trade_id;
};

struct Stats {
  std::uint64_t segments = 0, messages = 0, levels = 0, quotes = 0, trades = 0, other = 0, malformed = 0, gaps = 0;
};

inline std::uint64_t symbol_key(std::string_view s) noexcept {
  unsigned char b[8];
  std::memset(b, ' ', 8);
  std::memcpy(b, s.data(), s.size() < 8 ? s.size() : 8);
  return load_le<std::uint64_t>(b);
}

inline std::string symbol_str(std::uint64_t k) {
  char b[9];
  std::memcpy(b, &k, 8);
  b[8] = 0;
  std::size_t n = 8;
  while (n && b[n - 1] == ' ') --n;
  return std::string(b, n);
}

// Sequence tracking: messages are numbered per session; a jump means loss
// (in a capture: a hole in the file).
struct SeqState {
  std::int64_t next = 0;
  std::uint32_t session = 0;
};

// Parses one IEX-TP segment. H receives on_level / on_quote / on_trade.
template <class H>
bool parse_segment(const unsigned char* p, std::size_t n, H& h, Stats& st, SeqState* seq = nullptr) noexcept {
  if (n < kTpHeader || p[0] != 1) { st.malformed++; return false; }
  TpHeader t;
  t.protocol = load_le<std::uint16_t>(p + 2);
  t.channel = load_le<std::uint32_t>(p + 4);
  t.session = load_le<std::uint32_t>(p + 8);
  t.payload_len = load_le<std::uint16_t>(p + 12);
  t.msg_count = load_le<std::uint16_t>(p + 14);
  t.stream_offset = load_le<std::int64_t>(p + 16);
  t.first_seq = load_le<std::int64_t>(p + 24);
  t.send_time = load_le<std::int64_t>(p + 32);
  if (kTpHeader + t.payload_len > n) { st.malformed++; return false; }
  st.segments++;
  if (seq) {
    if (seq->session != t.session) {
      seq->session = t.session;
      seq->next = t.first_seq;
    }
    if (t.msg_count && t.first_seq != seq->next) st.gaps++;
    if (t.msg_count) seq->next = t.first_seq + t.msg_count;
  }
  const unsigned char* m = p + kTpHeader;
  const unsigned char* end = m + t.payload_len;
  for (std::uint16_t i = 0; i < t.msg_count; ++i) {
    if (end - m < 2) { st.malformed++; return false; }
    const std::uint16_t len = load_le<std::uint16_t>(m);
    m += 2;
    if (len == 0 || end - m < len) { st.malformed++; return false; }
    st.messages++;
    switch (m[0]) {
      case '8':
      case '5':
        if (len < 30) { st.malformed++; break; }
        h.on_level(PriceLevel{m[0] == '8', (m[1] & 1) != 0, load_le<std::int64_t>(m + 2), load_le<std::uint64_t>(m + 10),
                              load_le<std::uint32_t>(m + 18), load_le<std::int64_t>(m + 22)});
        st.levels++;
        break;
      case 'Q':
        if (len < 42) { st.malformed++; break; }
        h.on_quote(Quote{load_le<std::int64_t>(m + 2), load_le<std::uint64_t>(m + 10), load_le<std::uint32_t>(m + 18),
                         load_le<std::uint32_t>(m + 38), load_le<std::int64_t>(m + 22), load_le<std::int64_t>(m + 30)});
        st.quotes++;
        break;
      case 'T':
        if (len < 38) { st.malformed++; break; }
        h.on_trade(Trade{load_le<std::int64_t>(m + 2), load_le<std::uint64_t>(m + 10), load_le<std::uint32_t>(m + 18),
                         load_le<std::int64_t>(m + 22), load_le<std::int64_t>(m + 30)});
        st.trades++;
        break;
      default:
        st.other++;  // system event, security directory, trading status, auction, ...
    }
    m += len;
  }
  return true;
}

// ---------------------------------------------------------------- books from DEEP
// Fixed set of symbols (each side is a PriceLadder, 2 MB with prefaulting), keyed by
// a small open-addressing table so lookups don't allocate. Prices are kept in
// cents; sub-penny levels (stocks under $1) are counted and skipped.
class L2Books {
 public:
  static constexpr std::size_t kSlots = 1024;  // power of two, > max books

  explicit L2Books(std::size_t max_books, std::uint32_t levels_log2 = 12) : levels_log2_(levels_log2) {
    max_books_ = max_books < kSlots / 2 ? max_books : kSlots / 2;
    keys_.assign(kSlots, 0);
    idx_.assign(kSlots, -1);
  }
  L2Books(const L2Books&) = delete;
  L2Books& operator=(const L2Books&) = delete;

  // Restricts books to these symbols (allocated now). Without it, the first
  // max_books symbols seen get books.
  void track(std::string_view sym) { slot(symbol_key(sym), true); }

  struct Book {
    std::uint64_t symbol;
    BidLadder bids;
    AskLadder asks;
    bool consistent = true;
    std::uint64_t updates = 0;
    Book(std::uint64_t s, std::uint32_t lg) : symbol(s), bids(lg), asks(lg) {}
  };

  // Returns the book that changed, or nullptr (untracked symbol, sub-penny price).
  LLE_ALWAYS_INLINE Book* apply(const PriceLevel& u) noexcept {
    const int i = slot(u.symbol, !fixed_);
    if (i < 0) { untracked_++; return nullptr; }
    Book& b = *books_[static_cast<std::size_t>(i)];
    if (LLE_UNLIKELY(u.price % 100)) { subpenny_++; return nullptr; }
    const std::int64_t c = u.price / 100;
    if (u.bid) b.bids.set(c, u.size);
    else b.asks.set(c, u.size);
    b.consistent = u.complete;
    b.updates++;
    return &b;
  }

  Book* find(std::string_view sym) noexcept {
    const int i = slot(symbol_key(sym), false);
    return i < 0 ? nullptr : books_[static_cast<std::size_t>(i)].get();
  }
  std::size_t size() const noexcept { return books_.size(); }
  const Book& book(std::size_t i) const noexcept { return *books_[i]; }
  void fix() noexcept { fixed_ = true; }  // stop creating books on first sight
  std::uint64_t untracked() const noexcept { return untracked_; }
  std::uint64_t subpenny() const noexcept { return subpenny_; }

 private:
  LLE_ALWAYS_INLINE int slot(std::uint64_t key, bool create) {
    std::size_t h = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> 54) & (kSlots - 1);
    for (;;) {
      if (keys_[h] == key) return idx_[h];
      if (keys_[h] == 0) break;
      h = (h + 1) & (kSlots - 1);
    }
    if (!create || books_.size() >= max_books_) {
      if (books_.size() >= max_books_) fixed_ = true;
      return -1;
    }
    keys_[h] = key;
    idx_[h] = static_cast<int>(books_.size());
    books_.push_back(std::make_unique<Book>(key, levels_log2_));
    return idx_[h];
  }

  std::uint32_t levels_log2_;
  std::size_t max_books_;
  bool fixed_ = false;
  std::vector<std::uint64_t> keys_;
  std::vector<int> idx_;
  std::vector<std::unique_ptr<Book>> books_;
  std::uint64_t untracked_ = 0, subpenny_ = 0;
};

// ---------------------------------------------------------------- writer (tests, synthetic captures)
class SegmentWriter {
 public:
  explicit SegmentWriter(std::uint16_t protocol, std::uint32_t session = 1) : protocol_(protocol), session_(session) {}
  void level(bool bid, bool complete, std::int64_t ts, std::string_view sym, std::uint32_t size, std::int64_t price) {
    unsigned char m[30];
    m[0] = bid ? '8' : '5';
    m[1] = complete ? 1 : 0;
    put(m + 2, ts);
    put(m + 10, symbol_key(sym));
    put(m + 18, size);
    put(m + 22, price);
    add(m, sizeof(m));
  }
  void quote(std::int64_t ts, std::string_view sym, std::uint32_t bs, std::int64_t bp, std::int64_t ap, std::uint32_t as) {
    unsigned char m[42];
    m[0] = 'Q';
    m[1] = 0;
    put(m + 2, ts);
    put(m + 10, symbol_key(sym));
    put(m + 18, bs);
    put(m + 22, bp);
    put(m + 30, ap);
    put(m + 38, as);
    add(m, sizeof(m));
  }
  void trade(std::int64_t ts, std::string_view sym, std::uint32_t size, std::int64_t price, std::int64_t id) {
    unsigned char m[38];
    m[0] = 'T';
    m[1] = 0;
    put(m + 2, ts);
    put(m + 10, symbol_key(sym));
    put(m + 18, size);
    put(m + 22, price);
    put(m + 30, id);
    add(m, sizeof(m));
  }
  void raw(unsigned char type, std::size_t len) {
    std::vector<unsigned char> m(len, 0);
    m[0] = type;
    add(m.data(), len);
  }
  // Returns the finished segment and starts the next one.
  std::vector<unsigned char> flush(std::int64_t send_time = 0) {
    std::vector<unsigned char> s(kTpHeader + body_.size());
    s[0] = 1;
    s[1] = 0;
    put(&s[2], protocol_);
    put(&s[4], std::uint32_t{0x1});
    put(&s[8], session_);
    put(&s[12], static_cast<std::uint16_t>(body_.size()));
    put(&s[14], count_);
    put(&s[16], offset_);
    put(&s[24], seq_);
    put(&s[32], send_time);
    std::memcpy(s.data() + kTpHeader, body_.data(), body_.size());
    offset_ += static_cast<std::int64_t>(body_.size());
    seq_ += count_;
    body_.clear();
    count_ = 0;
    return s;
  }
  void skip(std::int64_t n) { seq_ += n; }  // simulate loss

 private:
  template <class T>
  static void put(unsigned char* p, T v) { std::memcpy(p, &v, sizeof(T)); }
  void add(const unsigned char* m, std::size_t n) {
    const std::uint16_t l = static_cast<std::uint16_t>(n);
    body_.insert(body_.end(), reinterpret_cast<const unsigned char*>(&l), reinterpret_cast<const unsigned char*>(&l) + 2);
    body_.insert(body_.end(), m, m + n);
    count_++;
  }
  std::uint16_t protocol_;
  std::uint32_t session_;
  std::vector<unsigned char> body_;
  std::uint16_t count_ = 0;
  std::int64_t offset_ = 0, seq_ = 1;
};

// Wraps UDP payloads in Ethernet/IPv4/UDP and a classic pcap (ns) file.
inline void pcap_header(std::vector<unsigned char>& out) {
  const std::uint32_t h[6] = {0xa1b23c4du, 0x00040002u, 0, 0, 65535, 1};
  out.insert(out.end(), reinterpret_cast<const unsigned char*>(h), reinterpret_cast<const unsigned char*>(h) + 24);
}
inline void pcap_udp(std::vector<unsigned char>& out, std::int64_t ts_ns, const std::vector<unsigned char>& payload,
                     std::uint16_t dport = 10378) {
  std::vector<unsigned char> f(42 + payload.size(), 0);
  store_be<std::uint16_t>(&f[12], 0x0800);
  f[14] = 0x45;
  store_be<std::uint16_t>(&f[16], static_cast<std::uint16_t>(28 + payload.size()));
  f[22] = 64;
  f[23] = 17;
  store_be<std::uint16_t>(&f[34], 10378);
  store_be<std::uint16_t>(&f[36], dport);
  store_be<std::uint16_t>(&f[38], static_cast<std::uint16_t>(8 + payload.size()));
  std::memcpy(&f[42], payload.data(), payload.size());
  const std::uint32_t r[4] = {static_cast<std::uint32_t>(ts_ns / 1'000'000'000), static_cast<std::uint32_t>(ts_ns % 1'000'000'000),
                              static_cast<std::uint32_t>(f.size()), static_cast<std::uint32_t>(f.size())};
  out.insert(out.end(), reinterpret_cast<const unsigned char*>(r), reinterpret_cast<const unsigned char*>(r) + 16);
  out.insert(out.end(), f.begin(), f.end());
}

}  // namespace lle::iex
