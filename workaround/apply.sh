#!/bin/bash
# Install the unlit_vs.spv workaround into a SteamVR installation.
#
# The shipped vertex shader reads its UBO (Set 0 / Binding 0) for the value it
# stores to gl_Layer. That read dereferences an address that is not a valid
# mapping and wedges the GPU (gfxhub page fault, SQC data). This patched module
# stores a constant 0 to gl_Layer and does not read the UBO, which stops the
# crash. It is a workaround, not a proper fix.
#
# Usage: ./apply.sh [SteamVR dir]
#   default SteamVR dir: ~/.local/share/Steam/steamapps/common/SteamVR
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
STEAMVR="${1:-$HOME/.local/share/Steam/steamapps/common/SteamVR}"
DST="$STEAMVR/resources/shaders/vulkan/unlit_vs.spv"

[ -f "$DST" ] || { echo "error: $DST not found (pass the SteamVR dir as \$1)" >&2; exit 1; }

if [ ! -f "$DST.orig" ]; then
    cp -f "$DST" "$DST.orig"
    echo "backed up shipped shader -> $DST.orig"
fi

cp -f "$HERE/unlit_vs.spv" "$DST"
echo "installed workaround $(md5sum "$HERE/unlit_vs.spv" | cut -d' ' -f1) -> $DST"
echo "restart SteamVR for it to take effect."
