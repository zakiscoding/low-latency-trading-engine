#pragma once
// Startup-time allocation for hot-path tables.
//
// Big tables (order map, pool, ladders) are mmap'd, optionally backed by
// transparent huge pages (fewer TLB misses on random access), and prefaulted
// with memset so no page fault ever lands on the hot path.
#include <sys/mman.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace lle {

struct MemoryPolicy {
  static inline std::atomic<bool> hugepages{true};
};

inline void* table_alloc(std::size_t bytes) {
  const std::size_t huge = 2u << 20;
  const std::size_t len = ((bytes + huge - 1) / huge) * huge;
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
  madvise(p, len, MemoryPolicy::hugepages.load() ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
#endif
  std::memset(p, 0, len);  // prefault
  return p;
}

inline void table_free(void* p, std::size_t bytes) {
  if (!p) return;
  const std::size_t huge = 2u << 20;
  munmap(p, ((bytes + huge - 1) / huge) * huge);
}

}  // namespace lle
