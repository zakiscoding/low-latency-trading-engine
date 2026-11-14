#!/usr/bin/env bash
# Runs the same jobs as .github/workflows/ci.yml inside the dev image.
set -euo pipefail
cd /src
jobs=${*:-"release asan tsan clang fuzz"}
for j in $jobs; do
  echo "=== $j ==="
  case $j in
    release) cmake -S . -B /build/rel -G Ninja -DCMAKE_CXX_COMPILER=g++-14 >/dev/null
             ninja -C /build/rel >/dev/null && /build/rel/lle_tests --gtest_brief=1 ;;
    asan)    cmake -S . -B /build/asan -G Ninja -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Debug \
               -DLLE_SANITIZE=address,undefined -DLLE_NATIVE=OFF >/dev/null
             ninja -C /build/asan lle_tests >/dev/null
             UBSAN_OPTIONS=halt_on_error=1 /build/asan/lle_tests --gtest_brief=1 ;;
    tsan)    cmake -S . -B /build/tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
               -DLLE_SANITIZE=thread -DLLE_NATIVE=OFF >/dev/null
             ninja -C /build/tsan lle_tests >/dev/null
             TSAN_OPTIONS=halt_on_error=1 setarch -R /build/tsan/lle_tests --gtest_brief=1 --gtest_filter='ShmRing*:ShmControl*:Pipeline*:Matching*' ;;
    clang)   cmake -S . -B /build/clang -G Ninja -DCMAKE_CXX_COMPILER=clang++ >/dev/null
             ninja -C /build/clang >/dev/null && /build/clang/lle_tests --gtest_brief=1 ;;
    fuzz)    cmake -S . -B /build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DLLE_FUZZ=ON -DLLE_TESTS=OFF -DLLE_NATIVE=OFF >/dev/null
             ninja -C /build/fuzz fuzz_itch fuzz_coinbase fuzz_ws >/dev/null
             mkdir -p /tmp/corpus-itch && /build/rel/itch_synth --messages 40 --out /tmp/corpus-itch/seed.itch 2>/dev/null || true
             secs=${FUZZ_SECONDS:-60}
             /build/fuzz/fuzz_itch -max_total_time=$secs -max_len=4096 /tmp/corpus-itch 2>&1 | tail -2
             mkdir -p /tmp/corpus-cb && /build/fuzz/fuzz_coinbase -max_total_time=$secs -max_len=8192 /tmp/corpus-cb engine/fuzz/corpus/coinbase 2>&1 | tail -2
             /build/fuzz/fuzz_ws -max_total_time=$secs -max_len=4096 2>&1 | tail -2 ;;
  esac
done
