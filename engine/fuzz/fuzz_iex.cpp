// libFuzzer target: pcap/pcapng reader, Ethernet/IP/UDP unwrap and the IEX-TP
// segment parser feeding DEEP books. The input is treated as a whole capture file
// and, independently, as one bare IEX-TP segment.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lle/iex.hpp"

namespace {
struct H {
  lle::iex::L2Books* books;
  void on_level(const lle::iex::PriceLevel& u) { books->apply(u); }
  void on_quote(const lle::iex::Quote&) {}
  void on_trade(const lle::iex::Trade&) {}
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static lle::iex::L2Books books(4);
  H h{&books};
  std::vector<unsigned char> buf(data, data + size);
  lle::iex::Stats st;
  lle::iex::SeqState seq;
  lle::iex::parse_segment(buf.data(), buf.size(), h, st, &seq);
  lle::iex::CaptureReader rd(buf.data(), buf.size());
  lle::iex::Frame f;
  for (int i = 0; rd.ok() && i < 10000 && rd.next(f); ++i) {
    const unsigned char* pl;
    std::size_t pn;
    if (lle::iex::udp_payload(f.data, f.len, pl, pn)) lle::iex::parse_segment(pl, pn, h, st, &seq);
  }
  return 0;
}
