#pragma once
// Small shared helpers: branch hints, cache-line size, big-endian loads/stores.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define LLE_LIKELY(x) __builtin_expect(!!(x), 1)
#define LLE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LLE_ALWAYS_INLINE inline __attribute__((always_inline))
#define LLE_NOINLINE __attribute__((noinline))
#define LLE_COLD __attribute__((cold, noinline))

namespace lle {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
LLE_ALWAYS_INLINE constexpr T bswap(T v) noexcept {
  if constexpr (sizeof(T) == 2) return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
  else if constexpr (sizeof(T) == 4) return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
  else if constexpr (sizeof(T) == 8) return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
  else return v;
}

// Loads go through memcpy so they are legal on unaligned, in-place buffers.
// GCC/Clang turn memcpy+bswap into a single movbe/bswap'd mov.
template <class T>
LLE_ALWAYS_INLINE T load_be(const void* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  if constexpr (sizeof(T) > 1) v = bswap(v);
  return v;
}

// 48-bit big-endian (ITCH timestamps).
LLE_ALWAYS_INLINE std::uint64_t load_be48(const void* p) noexcept {
  const auto* b = static_cast<const unsigned char*>(p);
  std::uint64_t hi = load_be<std::uint16_t>(b);
  std::uint64_t lo = load_be<std::uint32_t>(b + 2);
  return (hi << 32) | lo;
}

template <class T>
LLE_ALWAYS_INLINE void store_be(void* p, T v) noexcept {
  if constexpr (sizeof(T) > 1) v = bswap(v);
  std::memcpy(p, &v, sizeof(T));
}

LLE_ALWAYS_INLINE void store_be48(void* p, std::uint64_t v) noexcept {
  auto* b = static_cast<unsigned char*>(p);
  store_be<std::uint16_t>(b, static_cast<std::uint16_t>(v >> 32));
  store_be<std::uint32_t>(b + 2, static_cast<std::uint32_t>(v));
}

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

}  // namespace lle
