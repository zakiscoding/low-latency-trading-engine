// libFuzzer target: the in-place Coinbase JSON scanner.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lle/coinbase.hpp"
#include "lle/ladder.hpp"

namespace {
struct H : lle::coinbase::NullHandler {
  lle::BidLadder b{12};
  lle::AskLadder a{12};
  void on_l2(const lle::coinbase::L2Update& u) {
    if (u.bid) b.set(u.price % 1000000, u.qty); else a.set(u.price % 1000000, u.qty);
  }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static H h;
  std::vector<char> buf(data, data + size);
  lle::coinbase::Stats st;
  lle::coinbase::parse(buf.data(), buf.size(), h, st);
  return 0;
}
