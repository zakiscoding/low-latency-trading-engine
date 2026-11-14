#!/usr/bin/env bash
# Socket path vs AF_XDP on the same veth pair, inside a privileged container.
# The engine and paper exchange run in a network namespace behind veth "vb";
# the replayer sends from the root namespace through "va", so packets cross a
# real (virtual) device and the generic XDP hook, not loopback shortcuts.
#
# Usage (from the laptop): DOCKER_EXTRA=--privileged tools/dbuild.sh 'tools/xdp_veth_bench.sh /build/rel'
# Writes results/<host>/veth_<mode>.json for each mode. Generic (skb) XDP copies every
# packet, so on veth this measures the cost of the extra hook, not the zero-copy win;
# the real comparison needs a NIC with native XDP (see tuning/check-nic.sh).
set -euo pipefail
B=${1:-/build/rel}
OUT=${OUT:-/src/results/laptop}
RATE=${RATE:-100000}; MSGS=${MSGS:-1000000}
MODES=${MODES:-"spin uring xdp"}
mkdir -p "$OUT"

cleanup() { ip netns del lle-rx 2>/dev/null || true; ip link del va 2>/dev/null || true; }
trap cleanup EXIT
cleanup
ip netns add lle-rx
ip link add va type veth peer name vb
ip link set vb netns lle-rx
ip addr add 10.77.0.1/24 dev va && ip link set va up
ip netns exec lle-rx ip addr add 10.77.0.2/24 dev vb
ip netns exec lle-rx ip link set vb up
ip netns exec lle-rx ip link set lo up
# Bigger veth queues so bursts aren't dropped before the hook.
ip link set va txqueuelen 10000
NS="ip netns exec lle-rx"

for M in $MODES; do
  RX=$M; EXTRA=()
  if [ "$M" = xdp ]; then EXTRA=(--xdp-if vb --xdp-queue 0 --xdp-mode skb); fi
  $NS $B/paper_exchange --feed 10.77.0.2:31000 --orders 127.0.0.1:31002 --locates 1,2,3,4 --cpu 4 \
      --stats-json "$OUT/veth_$M.exchange.json" >/tmp/exch.log 2>&1 &
  EX=$!
  $NS $B/itch_engine --listen 10.77.0.2:31001 --exchange 127.0.0.1:31002 --rx "$RX" "${EXTRA[@]}" --cpu 2 \
      --locates 1,2,3,4 --json "$OUT/veth_$M.json" --label "veth_$M" 2>/tmp/engine.log &
  EN=$!
  sleep 1
  $B/itch_replay --engine 10.77.0.2:31001 --exchange-feed 10.77.0.2:31000 --messages "$MSGS" --symbols 4 \
      --rate "$RATE" --cpu 6 2>/tmp/replay.log
  sleep 1
  kill -INT $EN; wait $EN || true
  kill -INT $EX; wait $EX || true
  echo "=== $M"; cat /tmp/engine.log
done
