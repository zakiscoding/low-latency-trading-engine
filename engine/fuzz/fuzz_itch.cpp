// libFuzzer target: ITCH stream + MoldUDP64 parsing driving the real book and pipeline.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lle/pipeline.hpp"

using namespace lle;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static TscClock clk = TscClock::calibrate(5);
  static MarketBook* book = [] {
    auto* b = new MarketBook({.levels_log2 = 12, .order_map_log2 = 12, .order_pool = 2048});
    for (int i = 0; i < 4; ++i) b->add_instrument(static_cast<std::uint16_t>(i));
    return b;
  }();
  static CaptureSender tx;
  static NullTelemetry tel;
  static ItchEngine<MarketBook, CaptureSender>* eng =
      new ItchEngine<MarketBook, CaptureSender>(*book, tx, tel, StrategyParams{}, RiskLimits{}, clk);
  tx.sent.clear();
  // Exact-size copy so ASan sees any read past the end.
  std::vector<unsigned char> buf(data, data + size);
  if (size && (data[0] & 1)) {
    eng->begin_packet(rdtsc());
    eng->on_packet(buf.data(), buf.size());
  } else {
    itch::ParseStats st;
    itch::parse_stream(buf.data(), buf.size(), *eng, st);
  }
  return 0;
}
