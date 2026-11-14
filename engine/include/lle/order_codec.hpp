#pragma once
// Pre-serialized order messages.
//
// The wire format is a small fixed-layout binary "enter order" message modelled on
// OUCH. A template per (instrument, side) is built at startup with every constant
// field already in place; sending an order patches four fields (token, qty, price,
// send timestamp) with big-endian stores into a cache-line-aligned buffer, then
// hands that buffer straight to send(). No formatting, no allocation.
//
// Layout (64 bytes, one cache line):
//   0  u8   type 'O' enter, 'X' cancel
//   1  u8   side 'B'/'S'
//   2  u8   tif  'I' IOC / 'D' day
//   3  u8   reserved
//   4  u32  instrument id
//   8  u64  client order token
//   16 u32  quantity
//   20 i64  price (ticks)
//   28 u64  engine TSC at send (for exchange-side latency attribution)
//   36 i64  feed timestamp of the triggering message (ns)
//   44 u64  account id
//   52 [12] reserved, zero
#include <cstdint>
#include <cstring>

#include "lle/common.hpp"

namespace lle {

constexpr std::size_t kOrderMsgSize = 64;

namespace ord {
constexpr std::size_t kType = 0, kSide = 1, kTif = 2, kInstr = 4, kToken = 8, kQty = 16, kPrice = 20, kTsc = 28,
                      kFeedTs = 36, kAccount = 44;
}

struct alignas(kCacheLine) OrderTemplate {
  unsigned char bytes[kOrderMsgSize];

  void init(char type, Side side, char tif, std::uint32_t instrument, std::uint64_t account) noexcept {
    std::memset(bytes, 0, sizeof(bytes));
    bytes[ord::kType] = static_cast<unsigned char>(type);
    bytes[ord::kSide] = side == Side::Buy ? 'B' : 'S';
    bytes[ord::kTif] = static_cast<unsigned char>(tif);
    store_be<std::uint32_t>(bytes + ord::kInstr, instrument);
    store_be<std::uint64_t>(bytes + ord::kAccount, account);
  }

  LLE_ALWAYS_INLINE void patch(std::uint64_t token, std::uint32_t qty, std::int64_t price, std::uint64_t tsc,
                               std::int64_t feed_ts) noexcept {
    store_be<std::uint64_t>(bytes + ord::kToken, token);
    store_be<std::uint32_t>(bytes + ord::kQty, qty);
    store_be<std::int64_t>(bytes + ord::kPrice, price);
    store_be<std::uint64_t>(bytes + ord::kTsc, tsc);
    store_be<std::int64_t>(bytes + ord::kFeedTs, feed_ts);
  }
};
static_assert(sizeof(OrderTemplate) == 64);

// Read-only view used by the paper exchange.
struct OrderMsgView {
  const unsigned char* p;
  char type() const noexcept { return static_cast<char>(p[ord::kType]); }
  Side side() const noexcept { return p[ord::kSide] == 'B' ? Side::Buy : Side::Sell; }
  char tif() const noexcept { return static_cast<char>(p[ord::kTif]); }
  std::uint32_t instrument() const noexcept { return load_be<std::uint32_t>(p + ord::kInstr); }
  std::uint64_t token() const noexcept { return load_be<std::uint64_t>(p + ord::kToken); }
  std::uint32_t qty() const noexcept { return load_be<std::uint32_t>(p + ord::kQty); }
  std::int64_t price() const noexcept { return load_be<std::int64_t>(p + ord::kPrice); }
  std::uint64_t tsc() const noexcept { return load_be<std::uint64_t>(p + ord::kTsc); }
  std::int64_t feed_ts() const noexcept { return load_be<std::int64_t>(p + ord::kFeedTs); }
  std::uint64_t account() const noexcept { return load_be<std::uint64_t>(p + ord::kAccount); }
};

// Exchange -> engine execution report (32 bytes).
//   0 u8 type 'A' ack, 'F' fill, 'C' cancelled/expired, 'R' reject
//   1 u8 side, 2 u8 liquidity 'M'/'T', 3 u8 reject reason
//   4 u32 qty, 8 u64 token, 16 i64 price, 24 i64 fee (in 1e-6 quote units)
constexpr std::size_t kExecMsgSize = 32;
struct ExecReport {
  char type;
  Side side;
  char liquidity;
  char reason;
  std::uint32_t qty;
  std::uint64_t token;
  std::int64_t price;
  std::int64_t fee_micros;

  void encode(unsigned char* b) const noexcept {
    b[0] = static_cast<unsigned char>(type);
    b[1] = side == Side::Buy ? 'B' : 'S';
    b[2] = static_cast<unsigned char>(liquidity);
    b[3] = static_cast<unsigned char>(reason);
    store_be<std::uint32_t>(b + 4, qty);
    store_be<std::uint64_t>(b + 8, token);
    store_be<std::int64_t>(b + 16, price);
    store_be<std::int64_t>(b + 24, fee_micros);
  }
  static ExecReport decode(const unsigned char* b) noexcept {
    return ExecReport{static_cast<char>(b[0]), b[1] == 'B' ? Side::Buy : Side::Sell, static_cast<char>(b[2]),
                      static_cast<char>(b[3]), load_be<std::uint32_t>(b + 4), load_be<std::uint64_t>(b + 8),
                      load_be<std::int64_t>(b + 16), load_be<std::int64_t>(b + 24)};
  }
};

}  // namespace lle
