#!/usr/bin/env bash
# End-to-end UDP tick-to-trade run on one host: replayer -> (paper exchange, engine) -> orders -> exchange.
# Usage: tools/run_udp_bench.sh <build_dir> <rx_mode> <label> [rate] [messages] [extra engine args...]
set -euo pipefail
B=${1:-/build/rel}; RX=${2:-spin}; LABEL=${3:-run}; RATE=${4:-100000}; MSGS=${5:-1000000}
shift 5 || true
OUT=${OUT:-/src/results/laptop}
mkdir -p "$OUT"
ENGINE_CPU=${ENGINE_CPU:-2}; EXCH_CPU=${EXCH_CPU:-4}; REPLAY_CPU=${REPLAY_CPU:-6}
$B/paper_exchange --locates 1,2,3,4 --cpu $EXCH_CPU --stats-json "$OUT/$LABEL.exchange.json" >/tmp/exch.log 2>&1 &
EX=$!
$B/itch_engine --rx "$RX" --cpu $ENGINE_CPU --locates 1,2,3,4 --json "$OUT/$LABEL.json" --label "$LABEL" "$@" 2>/tmp/engine.log &
EN=$!
sleep 1
$B/itch_replay --messages "$MSGS" --symbols 4 --rate "$RATE" --cpu $REPLAY_CPU 2>/tmp/replay.log
sleep 1
kill -INT $EN; wait $EN || true
kill -INT $EX; wait $EX || true
cat /tmp/replay.log /tmp/engine.log
grep final /tmp/exch.log || true
