#!/usr/bin/env python3
"""smoke_controls.py — end-to-end gate of the inputs & runtime controls
(ticket 0031) against a real firmware image.

Boots `halo-emu -f <firmware>` headless with a fresh MRAM image, drives
the control socket (tcp://127.0.0.1:9562) and observes the effects over
the Lua REPL (tcp://127.0.0.1:9563):

    1. ping             control socket answers
    2. led?             LED PWM readout parses (duty/period/on)
    3. button click     frame.button.single callback fires
    4. button hold 1.2s frame.button.long callback fires
    5. battery set 82%  frame.battery_voltage() reflects the set value
                        within one 10 s firmware poll
    6. charger on/off   frame.battery_charging() follows immediately
                        (edge-triggered re-fetch, no poll wait)
    7. wdt-fire         watchdog NMI + warm reset: the firmware's
                        cold-boot magic check reboots again via the SE,
                        the REPL comes back, /lfs content survives

Note the Lua-callback plumbing: button events arm a Lua debug hook, so
the callback only runs while the VM executes something — the checks
pump the VM with print() statements after each injection.

Usage: tests/smoke_controls.py [-f firmware.bin] [--repl-port 9563]
                               [--ctl-port 9562]
Exit code 0 = all checks passed.
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
sys.path.insert(0, os.path.join(REPO, "tools"))
from repl_smoke import Repl  # noqa: E402
from fetch_firmware import CALIBRATION_VERSION, default_firmware  # noqa: E402


class Ctl:
    """Control-socket client: one text verb per line, `ok`/`err` reply."""

    def __init__(self, port, timeout=90.0):
        self.sock = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
        self.f = self.sock.makefile("rw", encoding="utf-8", newline="\n")

    def cmd(self, line):
        self.f.write(line + "\n")
        self.f.flush()
        reply = self.f.readline().strip()
        if not reply.startswith("ok"):
            raise AssertionError(f"ctl {line!r}: {reply}")
        return reply

    def close(self):
        self.sock.close()


def check(name, ok, detail=""):
    print(f"smoke_controls: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def pump_until(r, needle, tries=6, gap=0.7):
    """Pump the Lua VM (the button hook needs VM activity) and collect
    channel-0 output until `needle` shows up."""
    seen = []
    for _ in range(tries):
        r.send(0, b"print('pump')")
        for ch, p in r.frames(gap):
            if ch == 0:
                text = p.decode(errors="replace")
                seen.append(text)
                if needle in text:
                    return True, seen
    return False, seen


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=default_firmware())
    p.add_argument("--repl-port", type=int, default=9563)
    p.add_argument("--ctl-port", type=int, default=9562)
    p.add_argument("--boot-wait", type=float, default=20.0)
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"smoke_controls: firmware not found: {args.firmware}\n"
                 f"                fetch one first: "
                 f"tools/fetch_firmware.py {CALIBRATION_VERSION}")

    flash = tempfile.mktemp(prefix="halo-controls-smoke-", suffix=".img")
    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware, "--flash", flash, "--headless",
         "--repl-port", str(args.repl_port),
         "--ctl-port", str(args.ctl_port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    try:
        time.sleep(args.boot_wait)
        if proc.poll() is not None:
            sys.exit(f"smoke_controls: halo-emu exited early "
                     f"({proc.returncode})")

        ctl = Ctl(args.ctl_port)
        r = Repl(args.repl_port)
        time.sleep(1.0)
        r.send(0, b"\x03")  # break main.lua so its output stays quiet
        time.sleep(0.5)
        r.drain()

        # 1. control socket liveness
        check("ctl ping", ctl.cmd("ping") == "ok")

        # 2. LED readout parses
        reply = ctl.cmd("led?")
        fields = dict(kv.split("=") for kv in reply.split()[1:])
        check("led? readout", {"duty", "period", "on"} <= set(fields),
              reply)

        # 3. scripted click reaches frame.button.single
        out = r.lua("frame.button.single(function() print('SGL') end) "
                    "print('armed')")
        check("arm single-click callback", out == "armed", repr(out))
        ctl.cmd("button click")
        time.sleep(1.0)  # 400 ms double-click window + margin
        hit, seen = pump_until(r, "SGL")
        check("button click -> Lua single callback", hit, repr(seen))
        r.drain()  # leftover pump echoes

        # 4. scripted 1.2 s hold reaches frame.button.long
        out = r.lua("frame.button.single(nil) "
                    "frame.button.long(function() print('LNG') end) "
                    "print('armed')")
        check("arm long-press callback", out == "armed", repr(out))
        ctl.cmd("button hold 1200")  # >=1000 ms, below the 2 s level
        time.sleep(0.5)
        hit, seen = pump_until(r, "LNG")
        check("button 1.2s hold -> Lua long callback", hit, repr(seen))
        r.drain()  # leftover pump echoes
        r.lua("frame.button.long(nil) print('cleared')")

        # 5. battery level: reflected within one 10 s firmware poll
        reply = ctl.cmd("battery set 82%")
        want_mv = int(dict(kv.split("=")
                           for kv in reply.split()[1:])["mv"])
        got_mv = None
        deadline = time.monotonic() + 15  # one 10 s poll + margin
        while time.monotonic() < deadline:
            out = r.lua("print(frame.battery_voltage())")
            try:
                mv = float(out)  # Lua prints numbers as e.g. '4048.0'
            except (TypeError, ValueError):
                mv = -1
            if abs(mv - want_mv) <= 8:
                got_mv = mv
                break
            time.sleep(1.0)
        check("battery set 82% -> frame.battery_voltage()",
              got_mv is not None, f"want ~{want_mv}, last {out!r}")

        # 6. charger: edge-triggered, no poll wait
        ctl.cmd("charger on")
        time.sleep(1.0)
        out = r.lua("print(frame.battery_charging())")
        check("charger on -> frame.battery_charging()", out == "true",
              repr(out))
        ctl.cmd("charger off")
        time.sleep(1.0)
        out = r.lua("print(frame.battery_charging())")
        check("charger off -> not charging", out == "false", repr(out))

        # 7. wdt-fire: NMI + warm reset; the firmware's boot-time
        # halo_watchdog_has_fired() check then reboots once more via
        # the SE (the magic survives the warm reset only), and the
        # device comes back with /lfs intact.
        out = r.lua("f=frame.file.open('/wdt-smoke.txt','w') "
                    "f:write('survived') f:close() print('written')")
        check("write /lfs before wdt-fire", out == "written", repr(out))
        ctl.cmd("wdt-fire")
        dropped = any(ch == "EOF" for ch, _ in r.frames(30))
        check("wdt-fire reboots (client dropped)", dropped)
        r.close()

        deadline = time.monotonic() + 60
        r = None
        while time.monotonic() < deadline:
            time.sleep(2)
            try:
                r = Repl(args.repl_port, timeout=5)
                r.send(0, b"\x03")
                time.sleep(0.5)
                r.drain()
                if r.lua("print('up')", timeout=5) == "up":
                    break
                r.close()
                r = None
            except OSError:
                r = None
        check("REPL reachable after wdt-fire", r is not None)
        out = r.lua("f=frame.file.open('/wdt-smoke.txt','r') "
                    "print(f:read()) f:close()")
        check("/lfs persisted across the watchdog reboot",
              out == "survived", repr(out))

        print("smoke_controls: PASS — all checks green")
        ok = True
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if os.path.exists(flash):
            os.unlink(flash)
        if not ok:
            print("smoke_controls: FAIL", file=sys.stderr)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
