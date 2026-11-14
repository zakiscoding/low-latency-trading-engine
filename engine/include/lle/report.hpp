#pragma once
// Off-hot-path helpers: histogram summaries as JSON/text, CPU pinning, memory locking.
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "lle/histogram.hpp"
#include "lle/pipeline.hpp"
#include "lle/tsc.hpp"

namespace lle {

inline std::string hist_json(const Histogram& h, const TscClock* clk) {
  auto cv = [&](std::uint64_t v) { return clk ? clk->to_ns(v) : v; };
  std::ostringstream o;
  o << "{\"count\":" << h.count() << ",\"p50\":" << cv(h.percentile(50)) << ",\"p90\":" << cv(h.percentile(90))
    << ",\"p99\":" << cv(h.percentile(99)) << ",\"p999\":" << cv(h.percentile(99.9))
    << ",\"max\":" << cv(h.max()) << ",\"min\":" << cv(h.min()) << "}";
  return o.str();
}

// Compact bucket dump for the UI's histogram chart: [[upper_ns, count], ...] of non-empty buckets.
inline std::string hist_buckets_json(const Histogram& h, const TscClock* clk) {
  std::ostringstream o;
  o << "[";
  bool first = true;
  const auto& c = h.counts();
  for (int i = 0; i < Histogram::kBuckets; ++i) {
    if (!c[i]) continue;
    const std::uint64_t hi = Histogram::bucket_high(i);
    o << (first ? "" : ",") << "[" << (clk ? clk->to_ns(hi) : hi) << "," << c[i] << "]";
    first = false;
  }
  o << "]";
  return o.str();
}

inline std::string stages_json(const StageHists& s, const TscClock& clk, bool with_buckets = false) {
  std::ostringstream o;
  o << "{";
  for (int i = 0; i < kNumStages; ++i) {
    o << (i ? "," : "") << "\"" << kStageNames[i] << "\":" << hist_json(s.h[i], &clk);
  }
  if (with_buckets) {
    o << ",\"buckets\":{";
    for (int i = 0; i < kNumStages; ++i)
      o << (i ? "," : "") << "\"" << kStageNames[i] << "\":" << hist_buckets_json(s.h[i], &clk);
    o << "}";
  }
  o << "}";
  return o.str();
}

inline void print_stages(const StageHists& s, const TscClock& clk, FILE* f = stdout) {
  std::fprintf(f, "  %-14s %10s %8s %8s %8s %8s %10s\n", "stage(ns)", "count", "p50", "p99", "p99.9", "max", "");
  for (int i = 0; i < kNumStages; ++i) {
    const Histogram& h = s.h[i];
    if (!h.count()) continue;
    std::fprintf(f, "  %-14s %10llu %8llu %8llu %8llu %8llu\n", kStageNames[i],
                 static_cast<unsigned long long>(h.count()),
                 static_cast<unsigned long long>(clk.to_ns(h.percentile(50))),
                 static_cast<unsigned long long>(clk.to_ns(h.percentile(99))),
                 static_cast<unsigned long long>(clk.to_ns(h.percentile(99.9))),
                 static_cast<unsigned long long>(clk.to_ns(h.max())));
  }
}

// Pins the calling thread. Returns false (and keeps going) if the CPU isn't allowed.
inline bool pin_to_cpu(int cpu) {
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// Locks current and future pages so the hot path never takes a major page fault.
inline bool lock_memory() { return mlockall(MCL_CURRENT | MCL_FUTURE) == 0; }

inline std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}

inline std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
  return "unknown";
}

}  // namespace lle
