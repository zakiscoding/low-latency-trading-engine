// libFuzzer target: WebSocket frame parser, fed in two chunks like a socket would.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lle/ws.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  lle::ws::WsParser p;
  std::vector<unsigned char> buf;
  const std::size_t split = size ? data[0] % (size + 1) : 0;
  std::size_t fed = 0;
  for (int round = 0; round < 2; ++round) {
    const std::size_t k = round == 0 ? split : size - split;
    buf.insert(buf.end(), data + fed, data + fed + k);
    fed += k;
    for (;;) {
      std::size_t used = 0;
      lle::ws::FrameEvent ev;
      const int r = p.next(buf.data(), buf.size(), used, ev);
      if (r <= 0) break;
      volatile unsigned char sink = ev.len ? ev.data[ev.len - 1] : 0;
      (void)sink;
      buf.erase(buf.begin(), buf.begin() + static_cast<long>(used));
    }
  }
  return 0;
}
