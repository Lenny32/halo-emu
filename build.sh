#!/usr/bin/env bash
# build.sh — rebuild the halo emulator. Run ./init.sh once first (it fetches
# the host deps, the ARM toolchain and the pinned QEMU checkout).
#
# Outputs: qemu/build/qemu-system-arm, rom-stub/build/rom-stub-v1_2.bin
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

[ -e qemu/.git ] || { echo "build.sh: qemu/ missing — run ./init.sh" >&2; exit 1; }

[ -d deps/prefix/lib/pkgconfig ] && \
    export PKG_CONFIG_PATH="$PWD/deps/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# halo machine + peripheral models live in patches/, not in the checkout
(cd patches/files && find . -type f) | while read -r f; do
    mkdir -p "qemu/$(dirname "$f")"
    cmp -s "patches/files/$f" "qemu/$f" || cp "patches/files/$f" "qemu/$f"
done

ninja -C qemu/build qemu-system-arm
make -C rom-stub all test-fw
