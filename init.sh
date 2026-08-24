#!/usr/bin/env bash
# init.sh — fetch and build the halo QEMU fork (ticket 0025).
#
# Idempotent: re-running skips whatever is already done. Everything lands
# inside this directory (qemu/ checkout+build, deps/ local pixman fallback).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

QEMU_PIN=v11.1.0
QEMU_URL=https://gitlab.com/qemu-project/qemu.git
PIXMAN_PIN=pixman-0.44.2
PIXMAN_URL=https://gitlab.freedesktop.org/pixman/pixman.git

die() { echo "init.sh: error: $*" >&2; exit 1; }
note() { echo "init.sh: $*"; }

# --- host dependencies ------------------------------------------------------

need_apt=()
command -v ninja >/dev/null 2>&1 || need_apt+=(ninja-build)
command -v pkg-config >/dev/null 2>&1 || need_apt+=(pkg-config)
command -v make >/dev/null 2>&1 || need_apt+=(build-essential)
pkg-config --exists glib-2.0 2>/dev/null || need_apt+=(libglib2.0-dev)
pkg-config --exists pixman-1 2>/dev/null || need_apt+=(libpixman-1-dev)
pkg-config --exists sdl2 2>/dev/null || need_apt+=(libsdl2-dev)

if [ ${#need_apt[@]} -gt 0 ]; then
    note "missing packages: ${need_apt[*]}"
    if sudo -n true 2>/dev/null; then
        sudo apt-get install -y "${need_apt[@]}"
    elif [ -t 0 ]; then
        sudo apt-get install -y "${need_apt[@]}"
    else
        note "cannot sudo non-interactively; continuing with fallbacks"
    fi
fi

command -v ninja >/dev/null 2>&1 || die "ninja is required (apt: ninja-build)"
command -v pkg-config >/dev/null 2>&1 || die "pkg-config is required"
pkg-config --exists glib-2.0 || die "glib-2.0 dev files required (apt: libglib2.0-dev)"

# pixman: if the system package is absent, build a pinned static copy into
# deps/ (pixman is tiny and meson-only; meson comes from a local pip venv).
if ! pkg-config --exists pixman-1; then
    export PKG_CONFIG_PATH="$PWD/deps/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    if ! pkg-config --exists pixman-1; then
        note "building local pixman ($PIXMAN_PIN) into deps/prefix"
        mkdir -p deps
        [ -x deps/pyvenv/bin/meson ] || {
            python3 -m venv deps/pyvenv
            deps/pyvenv/bin/pip install --quiet meson
        }
        [ -e deps/pixman-src/meson.build ] || \
            git clone --depth 1 --branch "$PIXMAN_PIN" "$PIXMAN_URL" deps/pixman-src
        deps/pyvenv/bin/meson setup deps/pixman-build deps/pixman-src \
            --prefix="$PWD/deps/prefix" --libdir=lib \
            --default-library=static --buildtype=release \
            -Dtests=disabled -Ddemos=disabled
        ninja -C deps/pixman-build install
        pkg-config --exists pixman-1 || die "local pixman build failed"
    fi
fi

pkg-config --exists sdl2 || \
    note "warning: SDL2 dev files missing — building without the GUI display \
(fine for ticket 0025; needed from ticket 0029)"

# --- QEMU checkout, pinned --------------------------------------------------

if [ ! -e qemu/.git ]; then
    note "cloning QEMU $QEMU_PIN"
    git clone --depth 1 --branch "$QEMU_PIN" "$QEMU_URL" qemu
fi
git -C qemu rev-parse -q --verify "refs/tags/$QEMU_PIN" >/dev/null || \
    die "qemu/ exists but is not the pinned $QEMU_PIN checkout"

# --- apply the halo patches as a commit on branch 'halo' ---------------------

# overlay our files (copy only on change, to keep ninja re-runs no-ops)
(cd patches/files && find . -type f) | while read -r f; do
    mkdir -p "qemu/$(dirname "$f")"
    cmp -s "patches/files/$f" "qemu/$f" || cp "patches/files/$f" "qemu/$f"
done

if git -C qemu apply --check ../patches/qemu-build-integration.patch 2>/dev/null; then
    git -C qemu apply ../patches/qemu-build-integration.patch
elif git -C qemu apply --reverse --check ../patches/qemu-build-integration.patch 2>/dev/null; then
    : # already applied
else
    die "patches/qemu-build-integration.patch neither applies nor is applied"
fi

git -C qemu checkout -q -B halo
if ! git -C qemu diff --quiet || [ -n "$(git -C qemu status --porcelain)" ]; then
    git -C qemu add -A
    git -C qemu -c user.name="halo-emu init.sh" -c user.email="halo-emu@localhost" \
        commit -q -m "hw/arm: add halo machine (Alif Balletto B1) [$QEMU_PIN + emulator/patches]"
    note "committed halo patches onto branch 'halo'"
fi

# --- configure + build -------------------------------------------------------

if [ ! -f qemu/build/build.ninja ]; then
    (cd qemu && ./configure --target-list=arm-softmmu --disable-docs)
fi
ninja -C qemu/build qemu-system-arm

note "done: qemu/build/qemu-system-arm"
qemu/build/qemu-system-arm --version | head -1
