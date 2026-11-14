#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "lle/shm.hpp"

using namespace lle::shm;

TEST(ShmRing, ReadsWhatWasWritten) {
  TelemetryRing r("/tmp/lle-test-ring-1", 4);
  for (int i = 0; i < 10; ++i) r.push(kTrade, 1, 0, 0, i, i * 2, 0, 0, 0, 0);
  EXPECT_EQ(r.published_head(), 10u);
  Record x;
  for (std::uint64_t i = 0; i < 10; ++i) {
    ASSERT_TRUE(r.read(i, x));
    EXPECT_EQ(x.a, static_cast<std::int64_t>(i));
    EXPECT_EQ(x.b, static_cast<std::int64_t>(2 * i));
  }
}

TEST(ShmRing, OverwrittenRecordsAreDetected) {
  TelemetryRing r("/tmp/lle-test-ring-2", 4);  // 16 records
  for (int i = 0; i < 40; ++i) r.push(kTrade, 0, 0, 0, i, 0, 0, 0, 0, 0);
  Record x;
  EXPECT_FALSE(r.read(0, x));   // lapped
  EXPECT_FALSE(r.read(23, x));  // lapped
  EXPECT_TRUE(r.read(24, x));
  EXPECT_EQ(x.a, 24);
  EXPECT_TRUE(r.read(39, x));
}

// Concurrent reader never sees a torn record: payload fields are written as a
// function of the index, so a mix of two writes would fail the check. (TSan job.)
TEST(ShmRing, ConcurrentReaderSeesNoTornRecords) {
  TelemetryRing r("/tmp/lle-test-ring-3", 6);
  std::atomic<bool> done{false};
  std::uint64_t bad = 0, good = 0;
  std::thread reader([&] {
    std::uint64_t next = 0;
    Record x;
    while (!done.load() || next < r.published_head()) {
      const std::uint64_t h = r.published_head();
      if (next + 64 < h) next = h - 64;  // fell behind: skip ahead
      for (; next < h; ++next) {
        if (!r.read(next, x)) continue;
        const auto i = static_cast<std::int64_t>(next);
        if (x.a != i || x.b != i * 3 || x.c != -i || x.e != (i ^ 0x55)) bad++; else good++;
      }
    }
  });
  for (std::int64_t i = 0; i < 2'000'000; ++i) r.push(kStats, 0, 0, 0, i, i * 3, -i, 0, i ^ 0x55, 0);
  done = true;
  reader.join();
  EXPECT_EQ(bad, 0u);
  EXPECT_GT(good, 0u);
}

TEST(ShmControl, VersionChangeIsSeen) {
  ::unlink("/tmp/lle-test-ctl");  // a leftover file from an earlier run already has version > 0
  Control w("/tmp/lle-test-ctl", true);
  Control rd("/tmp/lle-test-ctl", false);
  EXPECT_FALSE(rd.changed());
  w.get()->kill.store(1);
  w.get()->version.fetch_add(1, std::memory_order_release);
  EXPECT_TRUE(rd.changed());
  EXPECT_EQ(rd.get()->kill.load(), 1u);
  EXPECT_FALSE(rd.changed());
}
