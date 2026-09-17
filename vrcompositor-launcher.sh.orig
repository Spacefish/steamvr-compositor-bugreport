#!/bin/bash

# verbose
#set -x

set -o pipefail
shopt -s failglob
set -u

BASENAME="$(basename "$0")"
ROOT="$(cd "$(dirname "$0")" && echo $PWD)"
ARCH="$(uname -m)"

# the vrcompositor executable has it's own logging to vrcompositor.txt
# this captures the script launch work and stdout/stderr
LOGFILE=vrcompositor-linux.txt

STEAM_BASE_FOLDER="${STEAM_BASE_FOLDER:-$(realpath ~/.steam/steam)}"
mkdir -p "${STEAM_BASE_FOLDER}/${STEAM_CLIENT_LOG_FOLDER:-logs}"

log () {
    ( echo "${BASENAME}[$$]: $*" >&2 ) || :
}

# Require LDLP scout runtime environment
# SteamVR is launching us via steam-runtime-launch-client, which is runtime neutral, so we are expected to have to relaunch
if [ "${VALVE_SKIP_RUNTIME_SAFETY-0}" = "1" ]; then
	log "WARNING: Skipping runtime relaunch (VALVE_SKIP_RUNTIME_SAFETY enabled)"
elif [ -n "${STEAM_RUNTIME-}" ]; then
	log "Detected scout LDLP runtime."
	# We can only start logging once we have srt-logger availability, so we miss capturing the first LDLP relaunch
	logger="${STEAM_RUNTIME}/amd64/usr/bin/srt-logger"
	# continue
elif [[ "${ARCH}" == x86_64* ]]; then
	log "Relaunch under scout LDLP runtime."
	# Prefer the LegacySteamRuntime compat tool (the supported modern path for
	# scout LDLP) if it is installed, and fall back to the legacy location
	# under ~/.steam/root otherwise.
	LEGACY_RUNTIME="${STEAM_BASE_FOLDER}/steamapps/common/LegacySteamRuntime/legacy-steam-runtime"
	if [ -x "${LEGACY_RUNTIME}" ]; then
		log exec "${LEGACY_RUNTIME}" "$0" "$@"
		exec "${LEGACY_RUNTIME}" "$0" "$@"
	fi
	log "LegacySteamRuntime compat tool not found at ${LEGACY_RUNTIME}, falling back to legacy scout path. This may fail under newer Steam client configurations."
	log exec "$HOME/.steam/root/ubuntu12_32/steam-runtime/run.sh" "$0" "$@"
	exec "$HOME/.steam/root/ubuntu12_32/steam-runtime/run.sh" "$0" "$@"
	# unreachable
else
	# unexpected
	log "No steam runtime detected, unknown arch ${ARCH} - continuing."
fi

if [ -x "${logger-}" ]; then
	# Prevent multiple opens through vrenv.sh relaunch
	if [ -z "${_STEAMVR_LOGGING_READY-}" ]; then
		# Send stdout to a subprocess that copies its stdin to the log file,
		# maybe the terminal, and maybe the systemd Journal; then send stderr
		# to the same place as stdout
		exec > >( exec "${logger}" --exec-fallback --filename=${LOGFILE} ) 2>&1
		export _STEAMVR_LOGGING_READY=1
	fi
else
	log "WARNING: ${logger-} not found, logging disabled"
fi

if [ -z "${STEAMVR_VRENV-}" ]; then
	log "Relaunch under vrenv."
	VRBINDIR=${ROOT}/..
	log exec "$VRBINDIR/vrenv.sh" "$0" "$@"
	exec "$VRBINDIR/vrenv.sh" "$0" "$@"
	# unreachable
fi

# Fedora changes default SDL behavior to give higher priority to Vulkan, which breaks us
export SDL_VIDEODRIVER=x11

# developer tricks: disabling the watchdog abort may help debugging
#export STEAMVR_DISABLE_THREAD_WATCHDOGS=1

# NOTE: the vrcompositor-launcher stub does a bit of env manipulation and syscap work
# NOTE: we do not support a launch under debugger here, see "WaitForDebugger" instead
log exec "$ROOT/vrcompositor-launcher" "$@"
exec "$ROOT/vrcompositor-launcher" "$@"
