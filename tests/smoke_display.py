#!/usr/bin/env python3
"""smoke_display.py — end-to-end smoke test of the CDC200 display model
(ticket 0029).

Boots the bare-metal display test firmware (rom-stub/test/fw_dispsmoke.c,
built with `make -C rom-stub test-fw`) on the halo machine headless, and
checks the whole display path against the UART markers plus two QMP
screendumps:

    1. boot markers: pmic-ok (TPS65132 @ I2C1 0x3E write+readback),
       dsi-ok (DSI_PHY_STATUS lock/stop-state), panel-ok (vga020 @ 0x54
       16-bit register write), display-on, scanline-ok (3 LINE IRQs),
       ready
    2. screendump at "ready": 256x256 frame showing the firmware's 8
       vertical color bars, scanned out of the DTCM framebuffer through
       its 0x58930000 global alias
    3. screendump after "display-off" (the firmware clears GLB_CTRL
       bit0 after a ~3 s splash-style hold): a blanked (black) panel

Exit code 0 = all checks passed.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
FW = os.path.join(REPO, "rom-stub", "build", "fw_dispsmoke.bin")

BOOT_MARKERS = ["pmic-ok", "dsi-ok", "panel-ok", "display-on",
                "scanline-ok", "ready"]

# fw_dispsmoke.c bar_rgb, 8 bars of 32px
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (32, 32, 32)]


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


class Qmp:
    def __init__(self, path, timeout=10.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""
        self._read_msg()
        self.command("qmp_capabilities")

    def _read_msg(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("QMP socket closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def command(self, name, **arguments):
        msg = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._read_msg()
            if "return" in resp:
                return resp["return"]
            if "error" in resp:
                raise RuntimeError(f"QMP {name}: {resp['error']['desc']}")


def read_ppm(path):
    """Parse a binary P6 PPM into (width, height, pixel bytes)."""
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        fail(f"screendump is not a P6 PPM: {data[:16]!r}")
    fields, pos = [], 2
    while len(fields) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, maxval = fields
    if maxval != 255:
        fail(f"unexpected PPM maxval {maxval}")
    return w, h, data[pos:pos + w * h * 3]


def pixel(pixels, w, x, y):
    off = (y * w + x) * 3
    return tuple(pixels[off:off + 3])


def wait_marker(proc, deadline_markers, timeout=30.0):
    """Read UART lines from the emulator until the wanted marker; any
    *-FAIL marker or EOF is fatal.  Returns the markers seen."""
    seen = []
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = proc.stdout.readline()
        if not line:
            fail(f"emulator exited; markers seen: {seen}")
        line = line.decode(errors="replace").strip()
        if line:
            print(f"  uart: {line}")
        if line.endswith("-FAIL"):
            fail(f"firmware reported {line}")
        seen.append(line)
        if line == deadline_markers[-1]:
            missing = [m for m in deadline_markers if m not in seen]
            if missing:
                fail(f"missing boot markers: {missing}")
            return seen
    fail(f"timed out waiting for {deadline_markers[-1]!r}; saw {seen}")


def check_bars(w, h, pixels):
    if (w, h) != (256, 256):
        fail(f"panel is {w}x{h}, expected 256x256")
    for i, want in enumerate(BARS):
        for x, y in ((i * 32 + 16, 32), (i * 32 + 16, 128),
                     (i * 32 + 16, 224)):
            got = pixel(pixels, w, x, y)
            if got != want:
                fail(f"bar {i} at ({x},{y}): got {got}, want {want}")
    print("PASS: 8 color bars match")


def check_blank(w, h, pixels):
    for x, y in ((16, 16), (128, 128), (240, 240)):
        got = pixel(pixels, w, x, y)
        if got != (0, 0, 0):
            fail(f"blanked panel at ({x},{y}): got {got}, want (0, 0, 0)")
    print("PASS: panel blanked after disable")


def main():
    if not os.path.exists(FW):
        fail(f"{FW} not built (make -C rom-stub test-fw)")

    tmp = tempfile.mkdtemp(prefix="halo-disp-")
    qmp_path = os.path.join(tmp, "qmp.sock")
    mram = os.path.join(tmp, "mram.img")
    dump = os.path.join(tmp, "frame.ppm")

    cmd = [HALO_EMU, "-f", FW, "--flash", mram, "--rom-stub", "none",
           "--ble-port", "0", "--headless",
           "--", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    try:
        wait_marker(proc, BOOT_MARKERS)
        qmp = Qmp(qmp_path)

        qmp.command("screendump", filename=dump)
        check_bars(*read_ppm(dump))

        wait_marker(proc, ["display-off"], timeout=30.0)
        qmp.command("screendump", filename=dump)
        check_blank(*read_ppm(dump))

        qmp.command("quit")
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    print("smoke_display: all checks passed")


if __name__ == "__main__":
    main()
