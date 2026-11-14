// Writes a seeded synthetic ITCH 5.0 day in NASDAQ's length-prefixed file format.
//   itch_synth --out data/synth.itch [--messages N] [--symbols K] [--seed S]
#include <cstdio>
#include <fstream>

#include "lle/args.hpp"
#include "lle/itch_writer.hpp"

int main(int argc, char** argv) {
  const lle::Args a(argc, argv);
  lle::itch::SynthConfig c;
  c.messages = static_cast<std::uint64_t>(a.num("messages", 1'000'000));
  c.symbols = static_cast<int>(a.num("symbols", 4));
  c.seed = static_cast<std::uint64_t>(a.num("seed", 42));
  const auto day = lle::itch::synth_day(c);
  const std::string out = a.str("out", "synth.itch");
  std::ofstream f(out, std::ios::binary);
  f.write(reinterpret_cast<const char*>(day.data()), static_cast<std::streamsize>(day.size()));
  std::fprintf(stderr, "wrote %zu bytes (%llu messages) to %s\n", day.size(),
               static_cast<unsigned long long>(c.messages), out.c_str());
  return 0;
}
