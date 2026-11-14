#!/usr/bin/env bash
# Runs the measured optimization ladder on a Linux box (the GPU PC's Ubuntu boot).
# Each step adds one change and writes results/<machine>/<step>.json.
#
#   tuning/run-ladder.sh <build_dir> <machine-name> [itch-file]
#
# Step 1 (code) is the bench_pipeline variant ladder plus the compiler ladder.
# Step 2 (OS) re-runs the UDP benchmark before and after ubuntu-tune.sh runtime
#   (the grub part needs a reboot between runs: run once untuned, reboot tuned, run again
#   with STEP=os-tuned).
# Step 3 (network) compares blocking / spin / busy-poll / io_uring receive.
set -euo pipefail
B=${1:?build dir}; M=${2:?machine name}; ITCH=${3:-}
OUT="$(cd "$(dirname "$0")/.." && pwd)/results/$M"
mkdir -p "$OUT"
HOT=${HOT_CORE:-4}; EXCH=${EXCH_CPU:-6}; REPLAY=${REPLAY_CPU:-8}
STEP=${STEP:-os-untuned}

if [ -n "$ITCH" ]; then
  "$B/bench_pipeline" --file "$ITCH" --messages 20000000 --symbols 8 --cpu "$HOT" --repeat 5 \
    --label "$M" --json "$OUT/pipeline_itch_real.json"
fi

run_udp() { # mode label extra...
  local mode=$1 label=$2; shift 2
  "$B/paper_exchange" --locates 1,2,3,4 --cpu "$EXCH" >/tmp/exch.log 2>&1 &
  local ex=$!
  "$B/itch_engine" --rx "$mode" --cpu "$HOT" --locates 1,2,3,4 --json "$OUT/$label.json" --label "$label" "$@" 2>/tmp/engine.log &
  local en=$!
  sleep 1
  "$B/itch_replay" --messages 1000000 --symbols 4 --rate 20000 --cpu "$REPLAY" 2>/dev/null
  sleep 1
  kill -INT "$en"; wait "$en" || true
  kill -INT "$ex"; wait "$ex" || true
  grep kernel_rx_to_send /tmp/engine.log
}

run_udp blocking "udp_${STEP}_blocking"
run_udp spin "udp_${STEP}_spin"
run_udp spin "udp_${STEP}_spin_busypoll" --busy-poll-us 50
run_udp uring "udp_${STEP}_uring"
python3 "$(dirname "$0")/../tools/summarize_results.py" "$OUT"
