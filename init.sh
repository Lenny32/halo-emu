#!/usr/bin/env bash
# Bootstrap every dependency the Halo desktop emulator needs (ticket 0001).
#
# Assembles a self-contained West workspace at emulator/src/halo-ws:
#
#   emulator/src/halo-ws/
#   ├── .venv/                 python env (west, pyelftools, cmake<4.4, ninja, ...)
#   ├── alif/                  halo-firmware, standalone clone pinned at HALO_FW_REV
#   ├── zephyr/ modules/ ...    fetched by `west update` from alif/west.yml
#   └── zephyr-sdk-0.16.5/     arm cross-compiler, only for `-b halo` regression checks
#
# The workspace is 100% independent: the firmware comes from its own clone
# (HALO_FW_URL / HALO_FW_REV below), never from a checkout elsewhere on the
# machine — no git worktrees, no shared object stores.
#
# Nothing is written outside emulator/ (see AGENTS.md) — the single exception is
# the host apt packages, which need sudo and are listed for approval first.
#
# Usage:
#   ./init.sh                 full bootstrap (interactive apt confirmation)
#   ./init.sh -y              assume yes for the apt install
#   ./init.sh --no-apt        skip the apt stage (only verify the tools exist)
#   ./init.sh --skip-sdk      skip the ~1 GB Zephyr SDK download
#   ./init.sh --32bit         also install multilib + i386 SDL (BOARD=native_sim)
#   ./init.sh --check         run the ticket-0001 health gate at the end
#   ./init.sh --check-only    only run the health gate on an existing workspace
#
# Safe to re-run: finished stages are detected and skipped, `west update` is
# incremental.
set -euo pipefail

EMU_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WS="$EMU_DIR/src/halo-ws"
VENV="$WS/.venv"
SDK_VERSION="0.16.5"
SDK_DIR="$WS/zephyr-sdk-$SDK_VERSION"
BOARD="${BOARD:-native_sim_64}"

# Firmware source: a standalone clone, pinned. Bump FW_REV (or export
# HALO_FW_REV) to move the emulator to a newer firmware; re-point an existing
# workspace with `git -C src/halo-ws/alif fetch && git -C src/halo-ws/alif
# checkout --detach <rev>`.
FW_URL="${HALO_FW_URL:-https://github.com/brilliantlabsAR/halo-firmware}"
FW_REV="${HALO_FW_REV:-d1a96459cb9bec1ac21e4e3a0b0279b6f9ab5859}"

ASSUME_YES=0
DO_APT=1
DO_SDK=1
DO_CHECK=0
CHECK_ONLY=0
WANT_32BIT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes)     ASSUME_YES=1 ;;
        --no-apt)     DO_APT=0 ;;
        --skip-sdk)   DO_SDK=0 ;;
        --32bit)      WANT_32BIT=1; BOARD=native_sim ;;
        --check)      DO_CHECK=1 ;;
        --check-only) DO_CHECK=1; CHECK_ONLY=1 ;;
        -h|--help)    awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' \
                          "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "init.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
skip() { printf '    \033[2m(already done: %s)\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. host packages
APT_PKGS=(git wget xz-utils file build-essential python3-dev python3-venv
          pkg-config libsdl2-dev)
[[ $WANT_32BIT -eq 1 ]] && APT_PKGS+=(gcc-multilib g++-multilib libsdl2-dev:i386)

host_packages() {
    step "Host packages (native_sim uses host gcc, not the Zephyr SDK)"

    local missing=()
    for p in "${APT_PKGS[@]}"; do
        dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'ok installed' \
            || missing+=("$p")
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        skip "${#APT_PKGS[@]} packages present"
        return
    fi

    if [[ $DO_APT -eq 0 ]]; then
        die "missing host packages and --no-apt was given:
    sudo apt-get install -y ${missing[*]}"
    fi
    command -v apt-get >/dev/null || die "not a Debian/Ubuntu host — install by hand: ${missing[*]}"

    info "missing: ${missing[*]}"
    info "this is the only step that writes outside emulator/ (needs sudo)"
    if [[ $ASSUME_YES -eq 0 ]]; then
        read -r -p "    run 'sudo apt-get install -y ${missing[*]}'? [y/N] " reply
        [[ "$reply" =~ ^[Yy]$ ]] || die "declined; install the packages and re-run"
    fi

    if [[ $WANT_32BIT -eq 1 ]] && ! dpkg --print-foreign-architectures | grep -qx i386; then
        info "enabling the i386 architecture (needed for libsdl2-dev:i386)"
        sudo dpkg --add-architecture i386
    fi
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends "${missing[@]}"
}

# --------------------------------------------------------- 2. workspace + alif clone
workspace() {
    step "West workspace at $WS"
    mkdir -p "$WS"

    if [[ -e "$WS/alif/west.yml" ]]; then
        skip "alif/ exists ($(git -C "$WS/alif" rev-parse --short HEAD 2>/dev/null || echo '?'))"
        return
    fi

    # Standalone clone, detached at the pin: the emulator never references a
    # firmware checkout elsewhere on the machine (no worktrees, no --reference),
    # so it keeps working wherever emulator/ lives or moves.
    info "cloning $FW_URL at ${FW_REV:0:8}"
    git clone "$FW_URL" "$WS/alif"
    git -C "$WS/alif" checkout --detach "$FW_REV"
}

# --------------------------------------------------------------------- 3. python env
python_env() {
    step "Python environment ($VENV)"
    if [[ ! -x "$VENV/bin/python" ]]; then
        python3 -m venv "$VENV"
    else
        skip "venv exists"
    fi
    if [[ -x "$VENV/bin/west" ]]; then
        skip "west installed ($("$VENV/bin/west" --version))"
        return
    fi
    # cmake is pinned < 4.4: this Zephyr 3.6-era tree trips a FindZephyr-sdk
    # parse error on newer CMake (see applications/halo/SETUP.md).
    "$VENV/bin/pip" install --upgrade pip
    "$VENV/bin/pip" install west pyelftools intelhex cryptography click cbor2 \
        'cmake<4.4' ninja
}

# ------------------------------------------------------------------- 4. west update
west_workspace() {
    step "west init -l alif && west update (multi-GB on first run)"
    export PATH="$VENV/bin:$PATH"

    if [[ ! -d "$WS/.west" ]]; then
        (cd "$WS" && west init -l alif)
    else
        skip ".west/ exists"
    fi
    (cd "$WS" && west update)

    local nsim="$WS/zephyr/boards/posix/native_sim"
    [[ -d "$nsim" ]] || die "native_sim not found at $nsim — the zephyr pin moved?"
    info "board present: zephyr/boards/posix/native_sim (hwmv1 layout, as expected)"
}

# --------------------------------------------------------------- 5. zephyr sdk (opt)
zephyr_sdk() {
    step "Zephyr SDK $SDK_VERSION (arm-zephyr-eabi — only for '-b halo' checks)"
    if [[ $DO_SDK -eq 0 ]]; then
        info "skipped (--skip-sdk); the emulator itself never needs it"
        return
    fi
    if [[ -d "$SDK_DIR/arm-zephyr-eabi" ]]; then
        skip "$SDK_DIR"
        return
    fi

    local host arch
    arch=$(uname -m)
    case "$(uname -s)-$arch" in
        Linux-x86_64)  host=linux-x86_64 ;;
        Linux-aarch64) host=linux-aarch64 ;;
        *) info "no minimal SDK tarball for $(uname -s)-$arch — skipping"; return ;;
    esac

    local tarball="zephyr-sdk-${SDK_VERSION}_${host}_minimal.tar.xz"
    local url="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}/${tarball}"
    info "downloading $tarball (~100 MB, plus ~1 GB of toolchain)"
    wget -q --show-progress -O "$WS/$tarball" "$url"
    tar -xf "$WS/$tarball" -C "$WS"
    rm -f "$WS/$tarball"
    # No '-c': registering the CMake package would write to ~/.cmake, outside
    # emulator/. Point builds at it with ZEPHYR_SDK_INSTALL_DIR instead.
    (cd "$SDK_DIR" && ./setup.sh -t arm-zephyr-eabi)
}

# ------------------------------------------------------------- 6. health gate (opt)
health_gate() {
    step "Health gate on $BOARD (ticket 0001 acceptance criteria)"
    export PATH="$VENV/bin:$PATH"
    export ZEPHYR_TOOLCHAIN_VARIANT=host

    local failed=0
    _gate() { # name  sample-path  expected-substring-in-output ("" = build only)
        local name="$1" sample="$2" expect="${3:-}"
        local d="$WS/build/check-$name" out
        info "building $sample"
        if ! (cd "$WS" && west build -p -d "$d" -b "$BOARD" "$WS/$sample" >"$d.log" 2>&1); then
            printf '    \033[1;31mFAIL\033[0m %s (build) — see %s\n' "$name" "$d.log"
            failed=1; return
        fi
        if [[ -z "$expect" ]]; then
            printf '    \033[1;32mOK\033[0m   %s (build only)\n' "$name"; return
        fi
        # native_sim binaries idle forever after main() returns; time-box them.
        out=$(timeout 15 "$d/zephyr/zephyr.exe" 2>&1 || true)
        if grep -qF "$expect" <<<"$out"; then
            printf '    \033[1;32mOK\033[0m   %s (ran, saw "%s")\n' "$name" "$expect"
        else
            printf '    \033[1;31mFAIL\033[0m %s — no "%s" in output:\n%s\n' \
                "$name" "$expect" "$(sed 's/^/        /' <<<"$out" | tail -20)"
            failed=1
        fi
    }

    _gate hello   zephyr/samples/hello_world             "Hello World!"
    _gate littlefs zephyr/samples/subsys/fs/littlefs     "boot count"
    if [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
        _gate display zephyr/samples/drivers/display     "Display sample"
    else
        info "no DISPLAY/WAYLAND_DISPLAY — building the display sample without running it"
        _gate display zephyr/samples/drivers/display     ""
    fi

    [[ $failed -eq 0 ]] || die "health gate failed — the emulator effort is blocked on it"
}

# ------------------------------------------------------------------------------ main
if [[ $CHECK_ONLY -eq 1 ]]; then
    [[ -x "$VENV/bin/west" ]] || die "no workspace at $WS — run ./init.sh first"
    health_gate
    printf '\n\033[1;32mHealth gate passed.\033[0m\n'
    exit 0
fi

host_packages
workspace
python_env
west_workspace
zephyr_sdk
[[ $DO_CHECK -eq 1 ]] && health_gate

cat <<MSG

$(printf '\033[1;32mWorkspace ready:\033[0m') $WS
  activate:  source $VENV/bin/activate
  build:     $EMU_DIR/build.sh          # BOARD=$BOARD
  re-point the firmware tree: git -C $WS/alif checkout <rev>
MSG
