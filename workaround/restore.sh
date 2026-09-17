#!/bin/bash
# Restore the shipped unlit_vs.spv, undoing the workaround.
#
# Usage: ./restore.sh [SteamVR dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
STEAMVR="${1:-$HOME/.local/share/Steam/steamapps/common/SteamVR}"
DST="$STEAMVR/resources/shaders/vulkan/unlit_vs.spv"

[ -f "$DST" ] || { echo "error: $DST not found (pass the SteamVR dir as \$1)" >&2; exit 1; }

if [ -f "$DST.orig" ]; then
    cp -f "$DST.orig" "$DST"
    echo "restored the backup at $DST.orig -> $DST"
else
    cp -f "$HERE/unlit_vs.spv.orig" "$DST"
    echo "restored the shipped shader from the bundle -> $DST"
fi
md5sum "$DST"
echo "restart SteamVR for it to take effect."
