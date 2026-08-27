#!/usr/bin/env python3
"""smoke_sensors.py — end-to-end gate of the I2C0 motion sensors
(ticket 0037) against a real firmware image.

Boots `halo-emu -f <firmware>` headless, injects samples over the
control socket (tcp://127.0.0.1:9562) and reads them back through the
Lua REPL (tcp://127.0.0.1:9563):

    1. accel/magn round-trip   the mg <-> raw-LSB conversion the control
                               socket documents (2 g range = 16384 LSB/g,
                               30 G range = 1000 LSB/gauss)
    2. frame.imu.raw()         reports exactly what was injected: the
                               BMA580 and QMC6308 models must answer the
                               chip-ID and status reads or the drivers
                               never probe (a plain register file does
                               not, which is why they have own models)
    3. frame.imu.direction()   the firmware remaps device axes into its
                               host frame (up = dev.X, right = -dev.Z,
                               forward = dev.Y), so 1 g on X reads level
                               and 1 g on Z reads roll -90
    4. tap single/double       injected on INT1 (gpio3.2) and delivered
                               to the Lua tap callback

Both sensors are lazy-init in the devicetree, so nothing touches them
until Lua asks — a boot log stays clean either way, which is exactly why
this gate exists.

Note the Lua-callback plumbing: like the button events in
smoke_controls.py, the tap callback only runs while the VM executes
something, so the checks pump the VM with print() after each injection.

Usage: tests/smoke_sensors.py [-f firmware.bin] [--repl-port 9563]
                              [--ctl-port 9562]
Exit code 0 = all checks passed.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tests"))
from repl_smoke import Repl  # noqa: E402
from smoke_controls import Ctl, pump_until  # noqa: E402

ACCEL_LSB_PER_G = 16384
MAGN_LSB_PER_GAUSS = 1000


def check(name, ok, detail=""):
    print(f"smoke_sensors: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def fields(reply):
    """`ok x=1 y=2 ...` -> {'x': '1', ...} (ignores bare tokens)."""
    return dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)


def lua_floats(r, expr, names):
    """Evaluate a Lua table expression and return the named fields."""
    fmt = " ".join(f"{n}=%.2f" for n in names)
    args = ", ".join(f"t.{n}" for n in names)
    out = r.lua(f"local t = {expr} "
                f"print(string.format('{fmt}', {args}))")
    got = fields("ok " + out)
    if set(got) != set(names):
        raise AssertionError(f"unparsable Lua reply: {out!r}")
    return {k: float(v) for k, v in got.items()}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"))
    p.add_argument("--repl-port", type=int, default=9563)
    p.add_argument("--ctl-port", type=int, default=9562)
    p.add_argument("--boot-wait", type=float, default=20.0)
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"smoke_sensors: firmware not found: {args.firmware}")

    flash = tempfile.mktemp(prefix="halo-sensors-smoke-", suffix=".img")
    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware, "--flash", flash, "--headless",
         "--repl-port", str(args.repl_port),
         "--ctl-port", str(args.ctl_port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    try:
        time.sleep(args.boot_wait)
        if proc.poll() is not None:
            sys.exit(f"smoke_sensors: halo-emu exited early "
                     f"({proc.returncode})")

        ctl = Ctl(args.ctl_port)
        r = Repl(args.repl_port)
        time.sleep(1.0)
        r.send(0, b"\x03")  # break main.lua so its output stays quiet
        time.sleep(0.5)
        r.drain()

        check("ctl ping", ctl.cmd("ping") == "ok")

        # 1. mg/mgauss -> raw LSB round-trip through the machine props
        ctl.cmd("accel 1000 -500 250")
        got = fields(ctl.cmd("accel?"))
        raw = [int(v) for v in got["raw"].split(",")]
        check("accel raw conversion",
              raw == [ACCEL_LSB_PER_G, -ACCEL_LSB_PER_G // 2,
                      ACCEL_LSB_PER_G // 4]
              and (got["x"], got["y"], got["z"]) == ("1000", "-500", "250"),
              str(got))

        ctl.cmd("magn 200 -300 400")
        got = fields(ctl.cmd("magn?"))
        raw = [int(v) for v in got["raw"].split(",")]
        check("magn raw conversion",
              raw == [200, -300, 400]
              and (got["x"], got["y"], got["z"]) == ("200", "-300", "400"),
              str(got))

        # 2. the drivers probe and report the injected sample
        out = r.lua(
            "local t = frame.imu.raw() "
            "print(string.format('ax=%.1f ay=%.1f az=%.1f "
            "cx=%.1f cy=%.1f cz=%.1f', t.accelerometer.x, "
            "t.accelerometer.y, t.accelerometer.z, t.compass.x, "
            "t.compass.y, t.compass.z))")
        got = fields("ok " + out)
        check("frame.imu.raw() sees the injected accelerometer (mg)",
              [float(got["ax"]), float(got["ay"]), float(got["az"])]
              == [1000.0, -500.0, 250.0], out)
        check("frame.imu.raw() sees the injected magnetometer (mgauss)",
              [float(got["cx"]), float(got["cy"]), float(got["cz"])]
              == [200.0, -300.0, 400.0], out)

        # 3. orientation in the firmware's host frame (up = device X)
        ctl.cmd("accel 1000 0 0")
        d = lua_floats(r, "frame.imu.direction()",
                       ["pitch", "roll", "heading"])
        check("1 g on device X reads worn-level",
              abs(d["pitch"]) < 0.5 and abs(d["roll"]) < 0.5, str(d))

        ctl.cmd("accel 0 0 1000")
        d = lua_floats(r, "frame.imu.direction()",
                       ["pitch", "roll", "heading"])
        check("1 g on device Z reads roll -90",
              abs(d["roll"] + 90.0) < 0.5, str(d))
        # heading is hard-coded 0.0 in the firmware (it needs host-side
        # hard-iron calibration), so assert that rather than a compass angle
        check("direction() heading is the firmware's fixed 0.0",
              d["heading"] == 0.0, str(d))

        # 4. taps arrive on INT1 (gpio3.2) and reach the Lua callback
        out = r.lua("frame.imu.tap_callback(function(k) "
                    "print('TAP:'..tostring(k)) end) print('armed')")
        check("arm tap callback", out == "armed", repr(out))
        r.drain()

        for kind in ("single", "double"):
            ctl.cmd(f"tap {kind}")
            time.sleep(0.3)
            hit, seen = pump_until(r, f"TAP:{kind}", tries=8)
            check(f"{kind} tap -> Lua tap callback", hit, repr(seen[-2:]))
            r.drain()

        print("smoke_sensors: PASS — all checks green")
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
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
