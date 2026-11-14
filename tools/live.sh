#!/usr/bin/env bash
# Runs the public demo stack on one host and restarts any piece that exits:
# Bitstamp engine (paper) + paper exchange + telemetry uplink to the Worker.
#   UPLINK=wss://<worker>/ingest SECRET_FILE=/secrets/ingest B=/build/rel tools/live.sh
set -u
B=${B:-/build/rel}
keep() { while true; do "$@"; echo "$(date -u +%FT%TZ) exited ($?): $1, restarting in 5s" >&2; sleep 5; done; }
keep "$B/paper_exchange" --source bitstamp --product BTC-USD --orders 127.0.0.1:31012 --feed 127.0.0.1:31010 \
  --latency-us 20000 --cpu "${EXCH_CPU:-4}" > /tmp/exchange.log 2>&1 &
keep "$B/crypto_engine" --venue bitstamp --product BTC-USD --exchange 127.0.0.1:31012 \
  --control /dev/shm/lle-control --cpu "${ENGINE_CPU:-2}" > /tmp/engine.log 2>&1 &
sleep 3
keep "${TELEMETRY:-/src/telemetry/telemetry}" --ring /dev/shm/lle-telemetry-crypto --control /dev/shm/lle-control \
  --venue bitstamp --product BTC-USD --uplink "$UPLINK" --uplink-secret-file "$SECRET_FILE" --hz "${HZ:-10}"
