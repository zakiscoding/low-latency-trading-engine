#pragma once
// NASDAQ TotalView-ITCH 5.0 decoding, in place.
//
// Messages are never copied: each accessor reads its big-endian field straight out
// of the receive buffer. A message is only dispatched if its length is at least
// the spec length for its type, so truncated or hostile input can't read past the
// buffer (the fuzz target checks this under ASan).
//
// Framing supported:
//  - Length-prefixed stream: [u16 len][msg]... (NASDAQ's binary sample files)
//  - MoldUDP64 packets: [10B session][u64 seq][u16 count] then count x [u16 len][msg]
#include <cstddef>
#include <cstdint>

#include "lle/common.hpp"

namespace lle::itch {

// Spec lengths (including the type byte); 0 = unknown type.
constexpr std::size_t spec_len(char t) noexcept {
  switch (t) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default: return 0;
  }
}

// Zero-copy views. Each holds only a pointer into the buffer.
struct MsgView {
  const unsigned char* p;
  char type() const noexcept { return static_cast<char>(p[0]); }
  std::uint16_t locate() const noexcept { return load_be<std::uint16_t>(p + 1); }
  std::uint16_t tracking() const noexcept { return load_be<std::uint16_t>(p + 3); }
  std::uint64_t timestamp_ns() const noexcept { return load_be48(p + 5); }
};

struct SystemEvent : MsgView {
  char event_code() const noexcept { return static_cast<char>(p[11]); }
};
struct StockDirectory : MsgView {
  const char* stock() const noexcept { return reinterpret_cast<const char*>(p + 11); }  // 8 bytes, space padded
};
struct AddOrder : MsgView {  // 'A' and 'F'
  std::uint64_t ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
  Side side() const noexcept { return p[19] == 'B' ? Side::Buy : Side::Sell; }
  std::uint32_t shares() const noexcept { return load_be<std::uint32_t>(p + 20); }
  const char* stock() const noexcept { return reinterpret_cast<const char*>(p + 24); }
  std::uint32_t price() const noexcept { return load_be<std::uint32_t>(p + 32); }
};
struct OrderExecuted : MsgView {  // 'E'
  std::uint64_t ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
  std::uint32_t shares() const noexcept { return load_be<std::uint32_t>(p + 19); }
  std::uint64_t match() const noexcept { return load_be<std::uint64_t>(p + 23); }
};
struct OrderExecutedWithPrice : OrderExecuted {  // 'C'
  bool printable() const noexcept { return p[31] == 'Y'; }
  std::uint32_t price() const noexcept { return load_be<std::uint32_t>(p + 32); }
};
struct OrderCancel : MsgView {  // 'X'
  std::uint64_t ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
  std::uint32_t shares() const noexcept { return load_be<std::uint32_t>(p + 19); }
};
struct OrderDelete : MsgView {  // 'D'
  std::uint64_t ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
};
struct OrderReplace : MsgView {  // 'U'
  std::uint64_t old_ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
  std::uint64_t new_ref() const noexcept { return load_be<std::uint64_t>(p + 19); }
  std::uint32_t shares() const noexcept { return load_be<std::uint32_t>(p + 27); }
  std::uint32_t price() const noexcept { return load_be<std::uint32_t>(p + 31); }
};
struct Trade : MsgView {  // 'P' (non-cross trade against a hidden order)
  std::uint64_t ref() const noexcept { return load_be<std::uint64_t>(p + 11); }
  Side side() const noexcept { return p[19] == 'B' ? Side::Buy : Side::Sell; }
  std::uint32_t shares() const noexcept { return load_be<std::uint32_t>(p + 20); }
  const char* stock() const noexcept { return reinterpret_cast<const char*>(p + 24); }
  std::uint32_t price() const noexcept { return load_be<std::uint32_t>(p + 32); }
};

// Handler concept (all optional via a base with empty defaults):
struct NullHandler {
  void on_system(const SystemEvent&) {}
  void on_directory(const StockDirectory&) {}
  void on_add(const AddOrder&) {}
  void on_executed(const OrderExecuted&) {}
  void on_executed_price(const OrderExecutedWithPrice&) {}
  void on_cancel(const OrderCancel&) {}
  void on_delete(const OrderDelete&) {}
  void on_replace(const OrderReplace&) {}
  void on_trade(const Trade&) {}
  void on_other(const MsgView&) {}
};

struct ParseStats {
  std::uint64_t messages = 0;
  std::uint64_t malformed = 0;
  std::uint64_t unknown = 0;
};

// Dispatches one message. Returns false if malformed (too short / unknown type).
template <class H>
LLE_ALWAYS_INLINE bool dispatch(const unsigned char* p, std::size_t len, H& h, ParseStats& st) noexcept {
  if (LLE_UNLIKELY(len == 0)) { st.malformed++; return false; }
  const char t = static_cast<char>(p[0]);
  const std::size_t need = spec_len(t);
  if (LLE_UNLIKELY(need == 0)) { st.unknown++; return false; }
  if (LLE_UNLIKELY(len < need)) { st.malformed++; return false; }
  st.messages++;
  switch (t) {
    case 'A':
    case 'F': h.on_add(AddOrder{{p}}); break;
    case 'E': h.on_executed(OrderExecuted{{p}}); break;
    case 'C': h.on_executed_price(OrderExecutedWithPrice{{{p}}}); break;
    case 'X': h.on_cancel(OrderCancel{{p}}); break;
    case 'D': h.on_delete(OrderDelete{{p}}); break;
    case 'U': h.on_replace(OrderReplace{{p}}); break;
    case 'P': h.on_trade(Trade{{p}}); break;
    case 'S': h.on_system(SystemEvent{{p}}); break;
    case 'R': h.on_directory(StockDirectory{{p}}); break;
    default: h.on_other(MsgView{p}); break;
  }
  return true;
}

// Parses a [u16 len][msg] stream. Returns bytes consumed (a trailing partial
// message is left unconsumed so the caller can append more data).
template <class H>
std::size_t parse_stream(const unsigned char* buf, std::size_t n, H& h, ParseStats& st) noexcept {
  std::size_t off = 0;
  while (off + 2 <= n) {
    const std::size_t len = load_be<std::uint16_t>(buf + off);
    if (off + 2 + len > n) break;
    dispatch(buf + off + 2, len, h, st);
    off += 2 + len;
  }
  return off;
}

struct MoldHeader {
  const unsigned char* p;
  const char* session() const noexcept { return reinterpret_cast<const char*>(p); }
  std::uint64_t seq() const noexcept { return load_be<std::uint64_t>(p + 10); }
  std::uint16_t count() const noexcept { return load_be<std::uint16_t>(p + 18); }
};
constexpr std::size_t kMoldHeader = 20;

// Parses one MoldUDP64 packet. Returns number of messages dispatched, or -1 if the
// header itself is truncated.
template <class H>
LLE_ALWAYS_INLINE int parse_mold(const unsigned char* buf, std::size_t n, H& h, ParseStats& st) noexcept {
  if (LLE_UNLIKELY(n < kMoldHeader)) { st.malformed++; return -1; }
  const MoldHeader mh{buf};
  const std::uint16_t count = mh.count();
  if (count == 0xFFFF) return 0;  // end-of-session marker
  std::size_t off = kMoldHeader;
  int k = 0;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (LLE_UNLIKELY(off + 2 > n)) { st.malformed++; break; }
    const std::size_t len = load_be<std::uint16_t>(buf + off);
    if (LLE_UNLIKELY(off + 2 + len > n)) { st.malformed++; break; }
    if (dispatch(buf + off + 2, len, h, st)) k++;
    off += 2 + len;
  }
  return k;
}

}  // namespace lle::itch
