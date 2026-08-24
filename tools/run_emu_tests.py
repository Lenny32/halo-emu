#!/usr/bin/env python3
# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0
"""Run the emulator-green subset of the device tests.

Launches zephyr.exe with a fresh flash file, points the unmodified tests in
applications/halo/tests/ at it through the brilliant_ble shim
(emulator/pyshim, activated via PYTHONPATH — zero test-file edits), and
reports pass/fail with the same failure-marker semantics as run_tests.py.

    emulator/tools/run_emu_tests.py               # the whole green subset
    emulator/tools/run_emu_tests.py --only test_time.py
    emulator/tools/run_emu_tests.py --no-launch   # emulator already running
    emulator/tools/run_emu_tests.py --list

Needs only python3 — no uv, no BLE stack, no PyPI packages (an import stub
covers test_time.py's unused luaparser dependency when it is not installed).
If the emulator exits mid-run (e.g. a reboot control code), it is relaunched
with the same flash file — the hardware "reboot" semantics.
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EMU_DIR = Path(__file__).resolve().parents[1]


def _fw_ws() -> Path:
    """The firmware west workspace, same resolution as build.sh:
    HALO_FW_WS env > emulator/.fw-ws sticky file > internal pinned clone."""
    env = os.environ.get("HALO_FW_WS")
    if env:
        return Path(env).resolve()
    sticky = EMU_DIR / ".fw-ws"
    if sticky.is_file():
        return Path(sticky.read_text().strip())
    return EMU_DIR / "src" / "halo-ws"


# Tests come from the same firmware tree the emulator was built from.
TESTS_DIR = _fw_ws() / "alif" / "applications" / "halo" / "tests"
PYSHIM_DIR = EMU_DIR / "pyshim"
DEFAULT_BINARY = EMU_DIR / "build" / "zephyr" / "halo-emu.exe"

# The emulator-green subset with the run_tests.py time limits: the M1 core
# (ticket 0006) plus the display tests (ticket 0007's fake CDC200 scanout,
# presented in an SDL window by ticket 0008 — test runs default to
# SDL_VIDEODRIVER=dummy in launch() so no window pops; export it yourself
# to override, e.g. SDL_VIDEODRIVER=x11 to watch the tests draw).
GREEN_TESTS = {
    "test_version.py": 45,
    "test_time.py": 120,
    "test_compression.py": 90,
    "test_file_api.py": 120,
    "test_file_execution.py": 120,
    "test_bluetooth_callback_api.py": 90,
    "test_display.py": 120,
    "test_display_bitmap.py": 200,
    "test_text_api.py": 200,
    # not registered in run_tests.py's table; measured well under this
    "test_display_palette.py": 120,
}

# Same convention as run_tests.py: exit 0 plus none of these in the output.
FAILURE_MARKERS = (
    "Lua error",
    "FAILED:",
    "Traceback (most recent call last)",
    "Not connected to device",
)

GREEN, RED, DIM, RESET = "\033[92m", "\033[91m", "\033[2m", "\033[0m"


def build_pythonpath():
    """PYTHONPATH for the test subprocesses: the shim, the luaparser import
    stub when the real package is absent, and whatever was already set."""
    entries = [str(PYSHIM_DIR)]
    probe = subprocess.run(
        [sys.executable, "-c", "import luaparser"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if probe.returncode != 0:
        entries.append(str(PYSHIM_DIR / "_stubs"))
    if os.environ.get("PYTHONPATH"):
        entries.append(os.environ["PYTHONPATH"])
    return os.pathsep.join(entries)


def wait_for_port(host, port, proc, timeout=30.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc is not None and proc.poll() is not None:
            sys.exit(f"emulator exited during startup (code {proc.returncode})")
        try:
            with socket.create_connection((host, port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    sys.exit(f"emulator never opened {host}:{port} within {timeout:.0f}s")


class Emulator:
    """The zephyr.exe under test; relaunched if it exits (reboot semantics)."""

    def __init__(self, binary, flash_file, host, port, log_path):
        self.cmd = [str(binary), f"--flash={flash_file}"]
        self.host, self.port = host, port
        self.log_path = log_path
        self.log = open(log_path, "ab")
        self.proc = None

    def launch(self):
        env = os.environ.copy()
        # Headless by default: since the 0008 presenter, the emulator opens
        # a real SDL window whenever a display server is reachable.
        env.setdefault("SDL_VIDEODRIVER", "dummy")
        self.proc = subprocess.Popen(
            self.cmd, cwd=Path(self.log_path).parent,
            stdout=self.log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, env=env,
        )
        wait_for_port(self.host, self.port, self.proc)

    def ensure_running(self):
        if self.proc.poll() is not None:
            print(f"{DIM}   emulator exited (code {self.proc.returncode}) — "
                  f"relaunching with the same flash file{RESET}")
            self.launch()

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


def run_one(name, timeout, env):
    """Run one test script; return (ok, seconds, detail, output)."""
    started = time.monotonic()
    try:
        proc = subprocess.run(
            [sys.executable, str(TESTS_DIR / name), "--name", "Halo 00"],
            cwd=TESTS_DIR, env=env, timeout=timeout,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        out, code, timed_out = proc.stdout, proc.returncode, False
    except subprocess.TimeoutExpired as e:
        out, code, timed_out = e.stdout or b"", None, True
    elapsed = time.monotonic() - started
    text = out.decode("utf-8", "replace")

    hits = sorted({m for m in FAILURE_MARKERS if m in text})
    if hits:
        return False, elapsed, f"printed {', '.join(repr(h) for h in hits)}", text
    if timed_out:
        return False, elapsed, f"timed out after {timeout}s", text
    if code != 0:
        return False, elapsed, f"exit {code}", text
    return True, elapsed, "", text


def main():
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--binary", type=Path, default=DEFAULT_BINARY,
                   help="zephyr.exe to launch (default: the emulator build)")
    p.add_argument("--port", type=int, default=9563,
                   help="TCP port of the Lua transport (default: 9563)")
    p.add_argument("--only", default="",
                   help="comma-separated subset of the green tests to run")
    p.add_argument("--flash", type=Path, default=None,
                   help="reuse/persist this flash file (default: a fresh "
                        "temp file, deleted afterwards)")
    p.add_argument("--no-launch", action="store_true",
                   help="do not start zephyr.exe; use the emulator already "
                        "listening at HALO_EMU_ADDR (or --port)")
    p.add_argument("--settle", type=float, default=1.0,
                   help="seconds between tests (default: 1)")
    p.add_argument("--list", action="store_true",
                   help="print the green subset and exit")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print each test's full output")
    args = p.parse_args()

    if args.list:
        for name, timeout in GREEN_TESTS.items():
            print(f"  {name:36s} limit {timeout}s")
        return 0

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    unknown = only - set(GREEN_TESTS)
    if unknown:
        p.error(f"not in the green subset: {', '.join(sorted(unknown))}")
    selected = [n for n in GREEN_TESTS if not only or n in only]

    env = dict(os.environ)
    env["PYTHONPATH"] = build_pythonpath()
    env.setdefault("HALO_EMU_ADDR", f"127.0.0.1:{args.port}")

    emu = None
    workdir = None
    if not args.no_launch:
        if not args.binary.exists():
            sys.exit(f"{args.binary} not found — build it first: emulator/build.sh")
        workdir = Path(tempfile.mkdtemp(prefix="halo-emu-tests-"))
        flash = args.flash.resolve() if args.flash else workdir / "flash.bin"
        emu = Emulator(args.binary.resolve(), flash,
                       "127.0.0.1", args.port, workdir / "emulator.log")
        print(f"{DIM}emulator: {' '.join(emu.cmd)}  (log: {emu.log_path}){RESET}")
        emu.launch()
    else:
        host, _, port = env["HALO_EMU_ADDR"].rpartition(":")
        wait_for_port(host or "127.0.0.1", int(port), None, timeout=5)

    results = []
    try:
        for name in selected:
            if emu:
                emu.ensure_running()
            timeout = GREEN_TESTS[name]
            print(f"{DIM}-- {name} (limit {timeout}s){RESET}")
            ok, secs, detail, text = run_one(name, timeout, env)
            results.append((name, ok, secs, detail))
            mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
            print(f"   {mark} {secs:5.1f}s {detail}")
            if args.verbose or not ok:
                shown = text.splitlines() if args.verbose else text.splitlines()[-15:]
                for line in shown:
                    print(f"      {DIM}{line}{RESET}")
            time.sleep(args.settle)
    finally:
        if emu:
            emu.stop()
            if workdir and not args.flash:
                shutil.rmtree(workdir, ignore_errors=True)

    print("\n" + "=" * 62)
    failed = [r for r in results if not r[1]]
    for name, ok, secs, detail in results:
        mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
        print(f"  {mark}  {name:36s} {secs:5.1f}s  {detail}")
    print(f"\n{len(results) - len(failed)}/{len(results)} passed"
          f"{', ' + str(len(failed)) + ' failed' if failed else ''}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
