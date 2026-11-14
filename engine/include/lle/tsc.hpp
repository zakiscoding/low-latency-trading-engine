#pragma once
// Cycle-counter helpers.
//  x86-64: the TSC. Assumes an invariant TSC (constant_tsc + nonstop_tsc), which
//          every x86 CPU from the last decade has; check /proc/cpuinfo.
//  arm64:  the generic timer's virtual counter (cntvct_el0). It ticks at a fixed
//          frequency (24 MHz on Apple M1, ~42 ns per tick), so stage timings are
//          much coarser than on x86; calibrate() measures the rate the same way.
#include <time.h>

#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#elif !defined(__aarch64__)
#error "lle supports x86-64 and arm64"
#endif

#include "lle/common.hpp"

namespace lle {

LLE_ALWAYS_INLINE std::uint64_t rdtsc_unordered() noexcept {
#if defined(__x86_64__)
  return __rdtsc();
#else
  std::uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#endif
}

// The counter read is not ordered against earlier loads; a fence before it keeps
// a stage's work from leaking past the stamp (lfence on x86, isb on arm64).
LLE_ALWAYS_INLINE std::uint64_t rdtsc() noexcept {
  asm volatile("" ::: "memory");  // compiler barrier: don't move memory ops across the stamp
#if defined(__x86_64__)
  _mm_lfence();
#else
  asm volatile("isb" ::: "memory");
#endif
  const std::uint64_t t = rdtsc_unordered();
  asm volatile("" ::: "memory");
  return t;
}

// Spin-wait hint.
LLE_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(__x86_64__)
  _mm_pause();
#else
  asm volatile("yield");
#endif
}

// Forces a value to be materialised before the next stamp (stops the compiler
// from sinking a computation past rdtsc()).
template <class T>
LLE_ALWAYS_INLINE void keep(const T& v) noexcept {
  asm volatile("" : : "r,m"(v) : "memory");
}


inline std::int64_t clock_ns(clockid_t id) noexcept {
  timespec ts;
  clock_gettime(id, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// Converts TSC cycles to nanoseconds with a 32.32 fixed-point multiplier, so the
// conversion on the reporting side is one multiply and one shift.
struct TscClock {
  double ticks_per_ns = 1.0;
  std::uint64_t mult_q32 = 1ull << 32;  // ns = cycles * mult >> 32
  std::uint64_t tsc0 = 0;
  std::int64_t realtime0 = 0;

  // Spins for `ms` milliseconds against CLOCK_MONOTONIC_RAW. Startup only.
  static TscClock calibrate(int ms = 200) noexcept {
    TscClock c;
    const std::int64_t m0 = clock_ns(CLOCK_MONOTONIC_RAW);
    const std::uint64_t t0 = rdtsc();
    std::int64_t m1;
    do { m1 = clock_ns(CLOCK_MONOTONIC_RAW); } while (m1 - m0 < ms * 1'000'000ll);
    const std::uint64_t t1 = rdtsc();
    c.ticks_per_ns = static_cast<double>(t1 - t0) / static_cast<double>(m1 - m0);
    c.mult_q32 = static_cast<std::uint64_t>((1.0 / c.ticks_per_ns) * 4294967296.0);
    c.tsc0 = rdtsc();
    c.realtime0 = clock_ns(CLOCK_REALTIME);
    return c;
  }

  LLE_ALWAYS_INLINE std::uint64_t to_ns(std::uint64_t cycles) const noexcept {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(cycles) * mult_q32) >> 32);
  }
  std::int64_t to_realtime_ns(std::uint64_t tsc) const noexcept {
    return realtime0 + static_cast<std::int64_t>(to_ns(tsc - tsc0));
  }
};

}  // namespace lle
