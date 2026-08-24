#!/usr/bin/env bash
# Build the Halo desktop emulator. See emulator/EMULATOR.md.
#
# Everything emulator-specific lives under emulator/ and is injected at
# build time (out-of-tree module + config/overlay fragments); the firmware
# tree is consumed read-only from the selected West workspace.
#
# Firmware source selection (which workspace's alif/ supplies the sources):
#   --fw-ws <path>   build from that west workspace (e.g. ~/halo-firmware,
#                    to test local — even uncommitted — firmware changes);
#                    remembered in emulator/.fw-ws for later runs
#   --fw-ws builtin  forget the remembered path; back to the self-contained
#                    pinned clone in src/halo-ws
#   HALO_FW_WS=<path>  one-shot override, nothing remembered
#
# Usage:
#   build.sh [--fw-ws <path>|builtin]     build for native_sim_64 (primary)
#   BOARD=native_sim build.sh             32-bit variant (multilib + i386 SDL)
set -euo pipefail

EMU_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WS="$EMU_DIR/src/halo-ws"          # internal workspace: toolchain + fallback sources
STICKY="$EMU_DIR/.fw-ws"
BOARD="${BOARD:-native_sim_64}"
BUILD_DIR="$EMU_DIR/build"

FW_WS_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fw-ws) [[ $# -ge 2 ]] || { echo "error: --fw-ws needs a path (or 'builtin')" >&2; exit 2; }
                 FW_WS_ARG="$2"; shift ;;
        -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' \
                       "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "build.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

# Resolve the firmware workspace: flag > env > sticky file > internal clone.
if [[ "$FW_WS_ARG" == "builtin" ]]; then
    rm -f "$STICKY"
    FW_WS="$WS"
elif [[ -n "$FW_WS_ARG" ]]; then
    FW_WS=$(readlink -f "$FW_WS_ARG")
elif [[ -n "${HALO_FW_WS:-}" ]]; then
    FW_WS=$(readlink -f "$HALO_FW_WS")
elif [[ -f "$STICKY" ]]; then
    FW_WS=$(<"$STICKY")
else
    FW_WS="$WS"
fi

APP="$FW_WS/alif/applications/halo"

if [[ ! -x "$WS/.venv/bin/west" ]]; then
    echo "error: no West workspace at $WS — run emulator/init.sh (ticket 0001)" >&2
    exit 1
fi
for need in "$FW_WS/.west" "$APP" "$FW_WS/zephyr/boards/posix/native_sim"; do
    if [[ ! -e "$need" ]]; then
        echo "error: '$FW_WS' is not a usable firmware workspace (missing $need)" >&2
        echo "       pass --fw-ws <path-to-west-workspace> or --fw-ws builtin" >&2
        exit 1
    fi
done
# Remember an explicitly passed workspace only once it validated.
[[ -n "$FW_WS_ARG" && "$FW_WS_ARG" != "builtin" ]] && printf '%s\n' "$FW_WS" > "$STICKY"

# Emulator toolchain (west/cmake<4.4/ninja) always comes from the internal
# venv, regardless of which workspace supplies the firmware sources.
export PATH="$WS/.venv/bin:$PATH"
# Host gcc, not the Zephyr SDK cross-compiler
export ZEPHYR_TOOLCHAIN_VARIANT=host

# Switching firmware workspaces reuses build/ — pristine it when the cached
# app source dir no longer matches, or CMake refuses the source change.
PRISTINE=()
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached=$(sed -n 's/^APPLICATION_SOURCE_DIR:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
    [[ "$cached" == "$(readlink -f "$APP")" ]] || PRISTINE=(-p)
fi

cd "$FW_WS"
# CONF_FILE (not EXTRA_CONF_FILE) carries the emulator fragment: the app's
# CMakeLists force-caches EXTRA_CONF_FILE=debug.conf for its debug/release
# switch, which would clobber anything passed there. debug.conf still
# appends after these files.
west build "${PRISTINE[@]}" -d "$BUILD_DIR" -b "$BOARD" "$APP" -- \
    -DZEPHYR_EXTRA_MODULES="$EMU_DIR/module" \
    -DCONF_FILE="$APP/prj.conf;$EMU_DIR/boards/native_sim.conf" \
    -DEXTRA_DTC_OVERLAY_FILE="$EMU_DIR/boards/native_sim.overlay"

# Convenience entry point (the real binary is build/zephyr/halo-emu.exe,
# named via CONFIG_KERNEL_BIN_NAME in boards/native_sim.conf)
ln -sf zephyr/halo-emu.exe "$BUILD_DIR/halo-emu"
FW_DESC=$(git -C "$FW_WS/alif" describe --tags --always --dirty 2>/dev/null || echo '?')
echo
echo "Built: $BUILD_DIR/halo-emu"
echo "  firmware: $FW_WS/alif @ $FW_DESC"
echo "  run it from $EMU_DIR so ./flash.bin is used"
