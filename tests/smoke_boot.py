#!/usr/bin/env python3
"""smoke_boot.py — headless boot smoke of a real firmware release
(ticket 0034).

Boots `halo-emu -f <firmware> --headless` on a throwaway MRAM image and
watches the console until the firmware's `main()` has finished bring-up:

    1. banner        the ASCII logo + Hardware/Firmware Version lines.
                     `main()` prints these behind CONFIG_HALO_LOG_LEVEL_DBG,
                     so only the -debug release asset has them; the check
                     is required for an image that contains the banner
                     string and skipped for one that does not
                     (--banner yes|no overrides the auto-detection)
    2. bring-up      power manager, BLE manager (i.e. the synthetic ROM
                     stub answered alif_ble_enable), Lua runtime
    3. main() done   "MCUboot image confirmed" — the last statement of
                     main() before it sleeps forever, so reaching it means
                     every subsystem initialised
    4. clean         no fault/assert/panic markers, and exactly one boot
                     (a watchdog reset would print the bring-up twice)
    5. teardown      SIGTERM to halo-emu leaves no qemu-system-arm behind
                     (ticket 0036's reaping — CI catches a regression here
                     instead of meeting it as a mystery flake later)

Usage:
    tests/smoke_boot.py -f firmwares/0.8.9/0.8.9.bin
    tests/smoke_boot.py --fetch latest --fetch-debug
Exit code 0 = all checks passed.
"""

import argparse
import os
import queue
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")

ANSI = re.compile(r"\x1b\[[0-9;]*m")

# Printed by main() itself, in order; the last one is main()'s final act.
BRINGUP = [
    ("power manager", "Power management initialized"),
    ("BLE manager", "BLE manager initialized"),
    ("Lua runtime", "Lua runtime initialized successfully"),
    ("main() complete", "MCUboot image confirmed"),
]

BANNER = [
    ("logo", "Copyright (C) 2025 Brilliant Labs"),
    ("hardware version", "Hardware Version:"),
    ("firmware version", "Firmware Version:"),
]
# Present in the image only when the banner printk is compiled in.
BANNER_SENTINEL = b"Hardware Version: "

FATAL = [
    "ZEPHYR FATAL ERROR",
    "BUS FAULT",
    "MEM FAULT",
    "MPU FAULT",
    "USAGE FAULT",
    "HARD FAULT",
    "ASSERTION FAIL",
    "Halting system",
    "<err> os:",
    "Watchdog timeout",
]


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def qemu_pids():
    """PIDs of halo QEMU processes, so we can tell ours from a stray."""
    out = subprocess.run(["pgrep", "-f", "[q]emu-system-arm"],
                         capture_output=True, text=True).stdout
    return {int(p) for p in out.split()}


def boot(cmd, timeout):
    """Run halo-emu until every bring-up marker is seen; return the log.

    The console goes quiet for seconds at a time (the Lua runtime takes
    ~10 s of guest time under TCG), so the deadline is enforced by the
    main thread while a reader thread drains the pipe — a firmware that
    hangs with nothing more to say still fails at `timeout` rather than
    blocking forever in readline().
    """
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1, text=True,
                            errors="replace")
    q = queue.Queue()

    def reader():
        for raw in proc.stdout:
            q.put(ANSI.sub("", raw).rstrip("\r\n"))
        q.put(None)

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    lines, pending = [], list(BRINGUP)
    deadline = time.monotonic() + timeout
    try:
        while pending:
            try:
                line = q.get(timeout=max(0.1, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:  # halo-emu exited
                break
            lines.append(line)
            if pending[0][1] in line:
                print(f"  ok  {pending.pop(0)[0]}")
            if time.monotonic() > deadline:
                break
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        t.join(timeout=5)
        while True:  # whatever the reader had already queued
            try:
                line = q.get_nowait()
            except queue.Empty:
                break
            if line is not None:
                lines.append(line)
        proc.stdout.close()
    if pending:
        tail = "\n    ".join(lines[-25:])
        why = "exited early" if proc.returncode is not None and \
            proc.returncode not in (0, -signal.SIGTERM) else \
            f"did not get there within {timeout:g}s"
        fail(f"boot never reached {pending[0][0]!r} "
             f"(marker {pending[0][1]!r}): {why}\n"
             f"  last console output:\n    {tail}")
    return lines


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   help="firmware image to boot (default: --fetch latest)")
    p.add_argument("--fetch", metavar="VERSION",
                   help="fetch and boot this release instead "
                        "(tag or 'latest')")
    p.add_argument("--fetch-debug", action="store_true",
                   help="with --fetch, take the -debug asset (the build "
                        "that prints the banner)")
    p.add_argument("--banner", choices=("auto", "yes", "no"), default="auto",
                   help="require the boot banner (default auto: required "
                        "iff the image contains the banner string)")
    p.add_argument("--timeout", type=float, default=180.0,
                   help="seconds to wait for main() to finish (default "
                        "180; TCG is a lot slower than silicon)")
    args = p.parse_args()

    if bool(args.firmware) == bool(args.fetch):
        if not args.firmware and not args.fetch:
            args.fetch = "latest"
        else:
            p.error("give exactly one of -f and --fetch")

    flash = tempfile.mktemp(prefix="halo-emu-smoke-boot-", suffix=".img")
    cmd = [HALO_EMU, "--headless", "--flash", flash,
           "--ctl-port", "0", "--repl-port", "0"]
    if args.fetch:
        cmd += ["--fetch", args.fetch]
        if args.fetch_debug:
            cmd.append("--fetch-debug")
    else:
        if not os.path.exists(args.firmware):
            fail(f"firmware not found: {args.firmware}")
        cmd += ["-f", args.firmware]

    before = qemu_pids()
    print(f"smoke_boot: {' '.join(cmd[1:])}")
    started = time.monotonic()
    try:
        lines = boot(cmd, args.timeout)
    finally:
        if os.path.exists(flash):
            os.unlink(flash)
    print(f"  ({time.monotonic() - started:.1f}s to main() completion)")

    log = "\n".join(lines)

    # 1. banner — only the debug build compiles the printk in.
    image_path = args.firmware
    if not image_path:
        m = re.search(r"^fetch-firmware: cached (.+)$", log, re.M)
        if not m:
            fail("could not tell which firmware --fetch used")
        image_path = m.group(1)
    if args.banner == "auto":
        want_banner = BANNER_SENTINEL in open(image_path, "rb").read()
    else:
        want_banner = args.banner == "yes"
    if want_banner:
        for what, marker in BANNER:
            if marker not in log:
                fail(f"boot banner: no {what} line ({marker!r}) on the "
                     f"console")
            print(f"  ok  banner {what}")
    else:
        print(f"  --  banner not compiled into "
              f"{os.path.basename(image_path)} (release build); skipped")

    # 2. nothing blew up on the way.
    for marker in FATAL:
        if marker in log:
            hit = next(ln for ln in lines if marker in ln)
            fail(f"fatal marker on the console: {hit.strip()}")
    print("  ok  no fault/assert/panic markers")

    # 3. exactly one boot: a reset loop would repeat the bring-up.
    boots = log.count(BRINGUP[0][1])
    if boots != 1:
        fail(f"expected exactly one boot, saw {boots} "
             f"{BRINGUP[0][1]!r} lines (reset loop?)")
    print("  ok  single boot, no reset loop")

    # 4. teardown (ticket 0036): SIGTERM must not orphan QEMU.
    for _ in range(20):
        stray = qemu_pids() - before
        if not stray:
            break
        time.sleep(0.5)
    else:
        fail(f"qemu-system-arm survived halo-emu's exit: pids {sorted(stray)}")
    print("  ok  no stray qemu-system-arm after teardown")

    print("smoke_boot: PASS")


if __name__ == "__main__":
    main()
