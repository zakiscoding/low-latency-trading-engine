#!/usr/bin/env bash
# Builds bench_pipeline four ways and runs the shipped variant (v5) on the same data:
#   o2      -O2, generic x86-64-v2, no LTO
#   o3n     -O3 -march=native
#   lto     + link-time optimisation
#   pgo     + profile-guided optimisation, trained on SYNTHETIC data, measured on the real file
set -euo pipefail
DATA=${DATA:-/data/itch_head600m.bin}; MSGS=${MSGS:-20000000}; CPU=${CPU:-3}; OUT=${OUT:-/src/results/laptop}
CXX=${CXX:-g++-14}
cfg() { cmake -S /src -B "$1" -G Ninja -DCMAKE_CXX_COMPILER=$CXX -DLLE_TESTS=OFF "${@:2}" >/dev/null && ninja -C "$1" bench_pipeline >/dev/null; }
cfg /build/c-o2  -DLLE_OPT_LEVEL=2 -DLLE_NATIVE=OFF -DLLE_LTO=OFF
cfg /build/c-o3n -DLLE_NATIVE=ON -DLLE_LTO=OFF
cfg /build/c-lto -DLLE_NATIVE=ON -DLLE_LTO=ON
rm -rf /build/pgo-prof && mkdir -p /build/pgo-prof
cfg /build/c-pgogen -DLLE_NATIVE=ON -DLLE_LTO=ON -DLLE_PGO=gen -DLLE_PGO_DIR=/build/pgo-prof
/build/c-pgogen/bench_pipeline --messages 5000000 --symbols 8 --variant v5 >/dev/null 2>&1
cfg /build/c-pgo -DLLE_NATIVE=ON -DLLE_LTO=ON -DLLE_PGO=use -DLLE_PGO_DIR=/build/pgo-prof
for b in o2 o3n lto pgo; do
  /build/c-$b/bench_pipeline --file "$DATA" --messages "$MSGS" --symbols 8 --cpu "$CPU" --variant v5 --repeat 3 \
    --label "$b" --json "$OUT/compiler_$b.json" 2>/dev/null | grep -E "msg/s|tick_to"
done
