#pragma once
// Fixed-capacity open-addressing hash map from a 64-bit order id to a 32-bit pool
// slot, plus the preallocated order pool it points into.
//  - Linear probing over 16-byte entries: a probe sequence stays in one or two
//    cache lines.
//  - Backward-shift deletion, so there are no tombstones and lookups don't slow
//    down over a trading day.
//  - All memory is allocated once at startup.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "lle/common.hpp"
#include "lle/memory.hpp"

namespace lle {

class OrderIdMap {
 public:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  // Identity hashing (slot = low bits of the id) suits exchange order refs, which
  // are assigned in increasing order: live orders cluster in a sliding window of
  // the table instead of being scattered across it, so lookups hit cache.
  // Fibonacci hashing is the general-purpose fallback for ids with no such order.
  enum class Hash : std::uint8_t { Identity, Fibonacci };
  static inline Hash default_hash = Hash::Identity;

  explicit OrderIdMap(std::uint32_t capacity_log2, Hash h = default_hash)
      : mask_((1ull << capacity_log2) - 1), shift_(64 - capacity_log2), hash_(h) {
    e_ = static_cast<Entry*>(table_alloc(sizeof(Entry) * (mask_ + 1)));
    for (std::uint64_t i = 0; i <= mask_; ++i) e_[i] = Entry{0, kEmpty, 0};
  }
  ~OrderIdMap() { table_free(e_, sizeof(Entry) * (mask_ + 1)); }
  OrderIdMap(const OrderIdMap&) = delete;
  OrderIdMap& operator=(const OrderIdMap&) = delete;

  LLE_ALWAYS_INLINE std::uint32_t find(std::uint64_t key) const noexcept {
    for (std::uint64_t i = slot(key);; i = (i + 1) & mask_) {
      const Entry& en = e_[i];
      if (en.val == kEmpty) return kEmpty;
      if (en.key == key) return en.val;
    }
  }

  // Inserts or overwrites. Returns false when the map is too full (load > 7/8).
  LLE_ALWAYS_INLINE bool insert(std::uint64_t key, std::uint32_t val) noexcept {
    if (LLE_UNLIKELY(size_ * 8 >= (mask_ + 1) * 7)) return false;
    for (std::uint64_t i = slot(key);; i = (i + 1) & mask_) {
      Entry& en = e_[i];
      if (en.val == kEmpty) {
        en.key = key;
        en.val = val;
        size_++;
        return true;
      }
      if (en.key == key) {
        en.val = val;
        return true;
      }
    }
  }

  LLE_ALWAYS_INLINE bool erase(std::uint64_t key) noexcept {
    std::uint64_t i = slot(key);
    for (;; i = (i + 1) & mask_) {
      if (e_[i].val == kEmpty) return false;
      if (e_[i].key == key) break;
    }
    // Backward-shift: pull later entries of the same cluster into the hole.
    std::uint64_t hole = i;
    for (std::uint64_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
      if (e_[j].val == kEmpty) break;
      const std::uint64_t home = slot(e_[j].key);
      // Move j into hole if its home is not in (hole, j] cyclically.
      const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
      if (!between) {
        e_[hole] = e_[j];
        hole = j;
      }
    }
    e_[hole].val = kEmpty;
    size_--;
    return true;
  }

  std::uint64_t size() const noexcept { return size_; }
  std::uint64_t capacity() const noexcept { return mask_ + 1; }

 private:
  struct Entry {
    std::uint64_t key;
    std::uint32_t val;
    std::uint32_t pad;
  };
  LLE_ALWAYS_INLINE std::uint64_t slot(std::uint64_t key) const noexcept {
    return hash_ == Hash::Identity ? (key & mask_) : ((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }
  Entry* e_ = nullptr;
  std::uint64_t mask_;
  int shift_;
  Hash hash_;
  std::uint64_t size_ = 0;
};

// Preallocated pool with an intrusive free list. T must be trivially copyable.
template <class T>
class Pool {
 public:
  explicit Pool(std::uint32_t capacity) : cap_(capacity) {
    items_ = static_cast<Slot*>(table_alloc(sizeof(Slot) * cap_));
    for (std::uint32_t i = 0; i < cap_; ++i) items_[i].next_free = i + 1;
    free_ = 0;
  }
  ~Pool() { table_free(items_, sizeof(Slot) * cap_); }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // Returns kNone when exhausted.
  static constexpr std::uint32_t kNone = 0xFFFFFFFFu;
  LLE_ALWAYS_INLINE std::uint32_t alloc() noexcept {
    if (LLE_UNLIKELY(free_ >= cap_)) return kNone;
    const std::uint32_t i = free_;
    free_ = items_[i].next_free;
    used_++;
    return i;
  }
  LLE_ALWAYS_INLINE void release(std::uint32_t i) noexcept {
    items_[i].next_free = free_;
    free_ = i;
    used_--;
  }
  LLE_ALWAYS_INLINE T& operator[](std::uint32_t i) noexcept { return items_[i].v; }
  LLE_ALWAYS_INLINE const T& operator[](std::uint32_t i) const noexcept { return items_[i].v; }
  std::uint32_t used() const noexcept { return used_; }
  std::uint32_t capacity() const noexcept { return cap_; }

 private:
  struct Slot {
    T v;
    std::uint32_t next_free;
  };
  Slot* items_ = nullptr;
  std::uint32_t cap_;
  std::uint32_t free_ = 0;
  std::uint32_t used_ = 0;
};

}  // namespace lle
