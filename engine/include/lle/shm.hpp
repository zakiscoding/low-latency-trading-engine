#pragma once
// Shared memory between the engine and the telemetry service (a separate Go
// process on other cores).
//
// TelemetryRing: single-producer, overwrite-on-full ring of 64-byte records. The
// engine never waits for the reader: if the reader falls behind, old records are
// overwritten and the reader notices the gap from the per-record sequence numbers
// (seqlock check), counting what it lost. Producer cost: ~10 plain stores plus
// two release stores, all to one cache line.
//
// ControlBlock: written by the telemetry service (admin commands), read by the
// engine once per loop iteration via a version counter; one relaxed load when
// nothing changed.
//
// Layout is fixed and little-endian (x86-64); telemetry/shm.go mirrors it.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "lle/common.hpp"

namespace lle::shm {

constexpr std::uint64_t kRingMagic = 0x4C4C4554454C3031ull;  // "LLETEL01"
constexpr std::uint64_t kCtrlMagic = 0x4C4C4543544C3031ull;  // "LLECTL01"

enum RecType : std::uint8_t {
  kTop = 1,
  kLevel = 2,      // part of a depth snapshot
  kTrade = 3,
  kOrder = 4,
  kFill = 5,
  kLatency = 6,
  kStats = 7,
  kSnapBegin = 8,
  kSnapEnd = 9,
};

struct alignas(64) Record {
  std::atomic<std::uint64_t> seq;  // index+1 when valid, 0 while being written
  std::uint8_t type;
  std::uint8_t side;
  std::uint16_t book;
  std::uint32_t u32;   // type-specific (e.g. snapshot level index, liquidity)
  std::int64_t a, b, c, d, e;  // type-specific payload
  std::int64_t ts_ns;  // engine realtime estimate
};
static_assert(sizeof(Record) == 64);

struct alignas(64) RingHeader {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t rec_size;
  std::uint64_t capacity;  // records, power of two
  std::uint64_t pid;
  alignas(64) std::atomic<std::uint64_t> head;  // next index to write
};
static_assert(sizeof(RingHeader) == 128);

inline void* map_file(const std::string& path, std::size_t bytes, bool create) {
  const int fd = ::open(path.c_str(), create ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
  if (fd < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
  if (create && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    throw std::runtime_error("ftruncate " + path);
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) throw std::runtime_error("mmap " + path);
  return p;
}

template <class T>
LLE_ALWAYS_INLINE void st(T& f, T v) noexcept { __atomic_store_n(&f, v, __ATOMIC_RELAXED); }
template <class T, class U>
LLE_ALWAYS_INLINE void st(T& f, U v) noexcept { __atomic_store_n(&f, static_cast<T>(v), __ATOMIC_RELAXED); }
template <class T>
LLE_ALWAYS_INLINE T ld(const T& f) noexcept { return __atomic_load_n(&f, __ATOMIC_RELAXED); }

class TelemetryRing {
 public:
  // Creates (or re-creates) the ring file. capacity_log2=16 -> 4 MiB.
  TelemetryRing(const std::string& path, std::uint32_t capacity_log2 = 16)
      : cap_(1ull << capacity_log2), mask_(cap_ - 1) {
    bytes_ = sizeof(RingHeader) + cap_ * sizeof(Record);
    base_ = map_file(path, bytes_, true);
    std::memset(base_, 0, bytes_);
    hdr_ = static_cast<RingHeader*>(base_);
    recs_ = reinterpret_cast<Record*>(static_cast<char*>(base_) + sizeof(RingHeader));
    hdr_->version = 1;
    hdr_->rec_size = sizeof(Record);
    hdr_->capacity = cap_;
    hdr_->pid = static_cast<std::uint64_t>(::getpid());
    hdr_->head.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    hdr_->magic = kRingMagic;  // last: readers wait for it
  }
  ~TelemetryRing() { ::munmap(base_, bytes_); }
  TelemetryRing(const TelemetryRing&) = delete;
  TelemetryRing& operator=(const TelemetryRing&) = delete;

  // Single producer only.
  LLE_ALWAYS_INLINE void push(RecType t, std::uint16_t book, std::uint8_t side, std::uint32_t u32, std::int64_t a,
                              std::int64_t b, std::int64_t c, std::int64_t d, std::int64_t e,
                              std::int64_t ts) noexcept {
    const std::uint64_t i = head_local_;
    Record& r = recs_[i & mask_];
    r.seq.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    // Payload stores are relaxed atomics (plain movs on x86) so the seqlock is
    // race-free under the C++ memory model and clean under TSan.
    st(r.type, t);
    st(r.side, side);
    st(r.book, book);
    st(r.u32, u32);
    st(r.a, a);
    st(r.b, b);
    st(r.c, c);
    st(r.d, d);
    st(r.e, e);
    st(r.ts_ns, ts);
    r.seq.store(i + 1, std::memory_order_release);
    head_local_ = i + 1;
    hdr_->head.store(i + 1, std::memory_order_release);
  }

  std::uint64_t head() const noexcept { return head_local_; }
  std::uint64_t capacity() const noexcept { return cap_; }

  // Reader side (used by tests; the real reader is Go). Returns false if the record
  // at index i was overwritten or is being written.
  bool read(std::uint64_t i, Record& out) const noexcept {
    const Record& r = recs_[i & mask_];
    const std::uint64_t s1 = r.seq.load(std::memory_order_acquire);
    if (s1 != i + 1) return false;
    out.type = ld(r.type);
    out.side = ld(r.side);
    out.book = ld(r.book);
    out.u32 = ld(r.u32);
    out.a = ld(r.a);
    out.b = ld(r.b);
    out.c = ld(r.c);
    out.d = ld(r.d);
    out.e = ld(r.e);
    out.ts_ns = ld(r.ts_ns);
    std::atomic_thread_fence(std::memory_order_acquire);
    return r.seq.load(std::memory_order_relaxed) == s1;
  }
  std::uint64_t published_head() const noexcept { return hdr_->head.load(std::memory_order_acquire); }

 private:
  std::uint64_t cap_, mask_;
  std::size_t bytes_;
  void* base_;
  RingHeader* hdr_;
  Record* recs_;
  std::uint64_t head_local_ = 0;
};

// Admin/control block. Fields after `version` are only read by the engine after it
// sees a new version (written last by the writer, with release semantics).
struct alignas(64) ControlBlock {
  std::uint64_t magic;
  std::atomic<std::uint64_t> version;
  std::atomic<std::uint32_t> kill;            // 1 = kill switch engaged
  std::atomic<std::uint32_t> trading_enabled; // 0 = strategy stopped (book still built)
  std::int64_t max_order_qty;
  std::int64_t max_position;
  std::int64_t max_notional_ticks;
  std::int64_t max_orders_per_sec;
  std::int64_t max_orders_per_day;
  std::int64_t max_daily_loss_ticks;
  std::int64_t imbalance_num;
  std::int64_t imbalance_den;
  std::int64_t order_qty;
  std::int64_t replay_request;  // incremented by admin; the replay driver watches it
  std::uint8_t pad[24];
};
static_assert(sizeof(ControlBlock) == 128);

class Control {
 public:
  explicit Control(const std::string& path, bool create) {
    cb_ = static_cast<ControlBlock*>(map_file(path, sizeof(ControlBlock), create));
    if (create && cb_->magic != kCtrlMagic) {
      std::memset(static_cast<void*>(cb_), 0, sizeof(ControlBlock));
      cb_->trading_enabled.store(1);
      cb_->magic = kCtrlMagic;
    }
  }
  ~Control() { ::munmap(cb_, sizeof(ControlBlock)); }
  ControlBlock* get() noexcept { return cb_; }

  // Engine side: returns true when a new version is visible.
  LLE_ALWAYS_INLINE bool changed() noexcept {
    const std::uint64_t v = cb_->version.load(std::memory_order_acquire);
    if (LLE_LIKELY(v == seen_)) return false;
    seen_ = v;
    return true;
  }

 private:
  ControlBlock* cb_;
  std::uint64_t seen_ = 0;
};

}  // namespace lle::shm
