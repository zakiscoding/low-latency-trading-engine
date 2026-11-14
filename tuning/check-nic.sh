#!/usr/bin/env bash
# Read-only: identifies the onboard NIC and whether AF_XDP (zero-copy) is realistic.
# Needed before ladder step 3b (AF_XDP). Prints nothing sensitive (no addresses).
set -uo pipefail
NIC=${1:-$(ip -o route get 1.1.1.1 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')}
echo "interface: $NIC"
lspci -nn | grep -iE 'ethernet|network' || true
DRV=$(ethtool -i "$NIC" 2>/dev/null | awk '/^driver:/{print $2}')
echo "driver: $DRV   $(ethtool -i "$NIC" 2>/dev/null | grep -E '^(version|firmware-version)')"
echo "queues: $(ethtool -l "$NIC" 2>/dev/null | awk '/Current/{f=1} f&&/Combined/{print $2; exit}')"
echo "timestamping:"; ethtool -T "$NIC" 2>/dev/null | sed -n '1,12p'
case "$DRV" in
  igc|igb|ixgbe|i40e|ice|mlx5_core|stmmac)
    echo "verdict: $DRV supports native XDP and AF_XDP zero-copy in mainline. AF_XDP step is worth doing." ;;
  r8169|r8125)
    echo "verdict: $DRV (Realtek) has no native XDP/AF_XDP zero-copy in mainline; AF_XDP would run in copy (generic) mode, slower than plain sockets. Skip AF_XDP on this NIC." ;;
  *)
    echo "verdict: unknown driver '$DRV'; check https://docs.kernel.org/networking/af_xdp.html driver list." ;;
esac
