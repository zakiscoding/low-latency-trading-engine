#!/usr/bin/env bash
# Installs/reloads the launchd agent. Run on the iMac: bash install.sh <lle dir>
set -euo pipefail
LLE=${1:?lle dir, e.g. ~/1tb-drive/lle}
LLE=$(cd "$LLE" && pwd -P)
mkdir -p "$LLE/logs"
P=~/Library/LaunchAgents/com.lle.live.plist
sed "s|__LLE__|$LLE|g" "$LLE/lle-live.plist.in" > "$P"
launchctl bootout "gui/$(id -u)/com.lle.live" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$P"
launchctl print "gui/$(id -u)/com.lle.live" | grep -E "state|pid" | head -3
