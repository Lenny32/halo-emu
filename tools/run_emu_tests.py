#!/usr/bin/env python3
"""run_emu_tests.py — run the unmodified Halo device test-suite against
the emulator (ticket 0030).

Launches `halo-emu` headless with a fresh MRAM image and executes the
device tests from a firmware checkout's `applications/halo/tests/`
directly with this Python interpreter, with `pyshim/` prepended to
PYTHONPATH so their `import brilliant_ble` resolves to the emulator
shim (TCP 9563) instead of the real phone library.  No test file is
modified; a test passes when it exits 0 without printing a failure
marker — the same smoke criterion as the suite's own run_tests.py.

Reboot semantics: control code 0x02 resets the machine in place (MRAM,
i.e. /lfs, persists).  Should QEMU exit anyway (e.g. a manual `quit`),
the runner relaunches `halo-emu` on the same MRAM image and continues.

Usage:
    tools/run_emu_tests.py -f 0.8.8.bin [--tests-dir .../tests]
                           [--only test_time.py,...] [--all]

The tests directory defaults to $HALO_FW_WS/applications/halo/tests.
Default selection is the M1 subset (green on the retired native_sim
emulator); --all runs every `auto` test in the suite's manifest.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

# The tests are uv scripts with their own dependency headers (luaparser,
# ...). Run them through uv when it exists so those resolve; PYTHONPATH
# entries precede the script env's site-packages, so the brilliant_ble
# shim still shadows the real package uv installs.
UV = shutil.which("uv")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
PYSHIM = os.path.join(REPO, "pyshim")

# The device tests that were green on native_sim (old ticket 0006 M1).
M1_SUBSET = [
    "test_version.py",
    "test_time.py",
    "test_compression.py",
    "test_file_api.py",
    "test_file_execution.py",
    "test_bluetooth_callback_api.py",
]

# Same smoke criterion as applications/halo/tests/run_tests.py.
FAILURE_MARKERS = (
    "Lua error",
    "FAILED:",
    "Traceback (most recent call last)",
    "Not connected to device",
)

TIMEOUTS = {  # generous: TCG is slower than silicon
    "test_version.py": 90,
    "test_time.py": 240,
    "test_compression.py": 180,
    "test_file_api.py": 240,
    "test_file_execution.py": 240,
    "test_bluetooth_callback_api.py": 180,
}
DEFAULT_TIMEOUT = 240

GREEN, RED, DIM, RESET = "\033[92m", "\033[91m", "\033[2m", "\033[0m"


def find_tests_dir(arg):
    if arg:
        return arg
    ws = os.environ.get("HALO_FW_WS")
    if ws:
        cand = os.path.join(ws, "applications", "halo", "tests")
        if os.path.isdir(cand):
            return cand
    return None


class Emulator:
    """halo-emu under our control, relaunched on exit (reboot = the
    machine resets in place, but a QEMU exit must not kill the run)."""

    def __init__(self, firmware, flash, port):
        self.firmware = firmware
        self.flash = flash
        self.port = port
        self.proc = None

    def launch(self):
        self.proc = subprocess.Popen(
            [HALO_EMU, "-f", self.firmware, "--flash", self.flash,
             "--headless", "--repl-port", str(self.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def ensure_running(self):
        if self.proc is None or self.proc.poll() is not None:
            if self.proc is not None:
                print(f"{DIM}runner: QEMU exited "
                      f"({self.proc.returncode}); relaunching on the "
                      f"same MRAM{RESET}")
            self.launch()
            time.sleep(20)  # boot to the Lua runtime

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def run_test(tests_dir, name, port, timeout):
    env = dict(os.environ,
               PYTHONPATH=PYSHIM + os.pathsep +
               os.environ.get("PYTHONPATH", ""),
               HALO_EMU_ADDR=f"127.0.0.1:{port}",
               PYTHONUNBUFFERED="1")
    started = time.monotonic()
    runner = [UV, "run"] if UV else [sys.executable]
    proc = subprocess.Popen(
        runner + [os.path.join(tests_dir, name), "--name", "Halo EMU"],
        cwd=tests_dir, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        timed_out = True
    elapsed = time.monotonic() - started
    text = (out or b"").decode("utf-8", "replace")

    hits = sorted({m for m in FAILURE_MARKERS if m in text})
    if timed_out:
        return False, elapsed, "timed out", text
    if proc.returncode != 0:
        return False, elapsed, f"exit code {proc.returncode}", text
    if hits:
        return False, elapsed, \
            "printed " + ", ".join(repr(h) for h in hits), text
    return True, elapsed, "", text


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"),
                   help="firmware image (default: ./0.8.8.bin)")
    p.add_argument("--tests-dir",
                   help="firmware checkout's applications/halo/tests "
                        "(default: $HALO_FW_WS/applications/halo/tests)")
    p.add_argument("--only",
                   help="comma-separated test file names to run")
    p.add_argument("--port", type=int, default=9563)
    p.add_argument("--keep-flash", metavar="IMG",
                   help="use (and keep) this MRAM image instead of a "
                        "fresh throwaway one")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print each test's output")
    args = p.parse_args()

    tests_dir = find_tests_dir(args.tests_dir)
    if not tests_dir or not os.path.isdir(tests_dir):
        sys.exit("run_emu_tests: tests directory not found — pass "
                 "--tests-dir or set HALO_FW_WS")
    if not os.path.exists(args.firmware):
        sys.exit(f"run_emu_tests: firmware not found: {args.firmware}")

    selection = (args.only.split(",") if args.only else M1_SUBSET)
    missing = [t for t in selection
               if not os.path.exists(os.path.join(tests_dir, t))]
    if missing:
        sys.exit(f"run_emu_tests: not in {tests_dir}: "
                 f"{', '.join(missing)}")

    if args.keep_flash:
        flash = args.keep_flash
    else:
        flash = tempfile.mktemp(prefix="halo-emu-tests-", suffix=".img")

    emu = Emulator(args.firmware, flash, args.port)
    results = []
    try:
        emu.ensure_running()
        for name in selection:
            emu.ensure_running()
            ok, secs, why, text = run_test(
                tests_dir, name, args.port,
                TIMEOUTS.get(name, DEFAULT_TIMEOUT))
            mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
            print(f"{mark} {name:34s} {secs:6.1f}s"
                  + (f"  {DIM}{why}{RESET}" if why else ""))
            if args.verbose or not ok:
                for line in text.rstrip().splitlines():
                    print(f"    {DIM}{line}{RESET}")
            results.append(ok)
    finally:
        emu.stop()
        if not args.keep_flash and os.path.exists(flash):
            os.unlink(flash)

    passed = sum(results)
    print(f"\nrun_emu_tests: {passed}/{len(results)} passed")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
