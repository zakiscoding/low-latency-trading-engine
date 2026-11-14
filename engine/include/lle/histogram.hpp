#pragma once
// Log-linear latency histogram (HdrHistogram-style, <1.6% bucket width).
// record() is a handful of instructions and never allocates, so it is safe on the
// hot path. Values are stored as raw units (cycles or ns); the caller converts.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>

#include "lle/common.hpp"

namespace lle {

class Histogram {
 public:
  static constexpr int kSubBits = 7;                 // values < 128 exact; then 64 buckets per octave
  static constexpr int kSub = 1 << kSubBits;
  static constexpr int kHalf = kSub / 2;
  static constexpr int kMaxExp = 40;                 // values up to ~2^46 units, larger clamp to last
  static constexpr int kBuckets = kSub + kMaxExp * kHalf;

  LLE_ALWAYS_INLINE void record(std::uint64_t v) noexcept {
    counts_[index(v)]++;
    total_++;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
  }

  static LLE_ALWAYS_INLINE int index(std::uint64_t v) noexcept {
    if (v < static_cast<std::uint64_t>(kSub)) return static_cast<int>(v);
    const int msb = 63 - std::countl_zero(v);          // >= kSubBits
    const int e = msb - kSubBits + 1;                   // >= 1
    if (LLE_UNLIKELY(e > kMaxExp)) return kBuckets - 1;
    const int sub = static_cast<int>(v >> e);           // in [kHalf, kSub)
    return kSub + (e - 1) * kHalf + (sub - kHalf);
  }
  // Upper edge of a bucket, used for percentile reporting (conservative).
  static std::uint64_t bucket_high(int i) noexcept {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const int j = i - kSub;
    const int e = j / kHalf + 1;
    const std::uint64_t sub = static_cast<std::uint64_t>(j % kHalf + kHalf);
    return (sub << e) + ((1ull << e) - 1);
  }

  std::uint64_t count() const noexcept { return total_; }
  std::uint64_t max() const noexcept { return total_ ? max_ : 0; }
  std::uint64_t min() const noexcept { return total_ ? min_ : 0; }

  std::uint64_t percentile(double p) const noexcept {
    if (total_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(total_) + 0.5);
    const std::uint64_t want = std::max<std::uint64_t>(1, std::min(target, total_));
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen >= want) return i == kBuckets - 1 ? max_ : std::min(bucket_high(i), max_);
    }
    return max_;
  }

  void merge(const Histogram& o) noexcept {
    for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    total_ += o.total_;
    max_ = std::max(max_, o.max_);
    min_ = std::min(min_, o.min_);
  }
  void reset() noexcept { *this = Histogram{}; }

  const std::array<std::uint64_t, kBuckets>& counts() const noexcept { return counts_; }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace lle
