#pragma once
// Tiny --key value argument parser for the command-line tools.
#include <signal.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace lle {

class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string k = argv[i];
      if (k.rfind("--", 0) != 0) continue;
      k = k.substr(2);
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv_[k] = argv[++i];
      else kv_[k] = "1";
    }
  }
  std::string str(const std::string& k, const std::string& def = "") const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  long long num(const std::string& k, long long def = 0) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::stoll(it->second);
  }
  double dbl(const std::string& k, double def = 0) const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : std::stod(it->second);
  }
  bool flag(const std::string& k) const { return kv_.count(k) > 0; }
  std::vector<std::uint16_t> locates(const std::string& k, int default_count) const {
    std::vector<std::uint16_t> v;
    const std::string s = str(k);
    if (s.empty()) {
      for (int i = 1; i <= default_count; ++i) v.push_back(static_cast<std::uint16_t>(i));
      return v;
    }
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) v.push_back(static_cast<std::uint16_t>(std::stoi(t)));
    return v;
  }

 private:
  std::map<std::string, std::string> kv_;
};

inline volatile std::sig_atomic_t g_stop = 0;
// No SA_RESTART: a blocking recvmsg() returns EINTR so the loop can see g_stop.
inline void install_stop_handler() {
  struct sigaction sa{};
  sa.sa_handler = [](int) { g_stop = 1; };
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace lle
