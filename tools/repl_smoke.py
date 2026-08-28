#!/usr/bin/env python3
"""repl_smoke.py — end-to-end smoke test of the Lua REPL bridge
(ticket 0030) against a real firmware image.

Boots `halo-emu -f <firmware>` headless with a fresh MRAM image and
drives tcp://127.0.0.1:9563 with the native_sim wire protocol
([u8 channel][u16 LE length][payload]):

    1. REPL echo         print('...') comes back on channel 0
    2. MTU               frame.bluetooth.max_length() == 511
    3. data channel      0x01-marked writes reach
                         frame.bluetooth.receive_callback; replies come
                         back 0x01-marked via frame.bluetooth.send
    4. single client     a second connection is refused (immediate EOF)
    5. control codes     0x03 interrupts a busy loop, 0x04 restarts the
                         Lua VM, 0x02 reboots the machine in place —
                         the bridge drops the client, and after
                         reconnecting /lfs content written before the
                         reboot is still there

Usage: tools/repl_smoke.py [-f firmware.bin] [--port 9563]
Exit code 0 = all checks passed.
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")


class Repl:
    """Minimal wire-protocol client."""

    def __init__(self, port, timeout=20):
        self.sock = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
        self.buf = b""

    def send(self, channel, payload):
        self.sock.sendall(struct.pack("<BH", channel, len(payload)) +
                          payload)

    def frames(self, timeout=8.0):
        """Yield (channel, payload) until `timeout` of silence or EOF
        (EOF yields ('EOF', b''))."""
        self.sock.settimeout(timeout)
        try:
            while True:
                while len(self.buf) >= 3:
                    ch, ln = struct.unpack("<BH", self.buf[:3])
                    if len(self.buf) < 3 + ln:
                        break
                    payload = self.buf[3:3 + ln]
                    self.buf = self.buf[3 + ln:]
                    yield ch, payload
                data = self.sock.recv(65536)
                if not data:
                    yield "EOF", b""
                    return
                self.buf += data
        except (socket.timeout, ConnectionResetError):
            return

    def drain(self, quiet=1.0):
        for _ in self.frames(quiet):
            pass

    def lua(self, code, timeout=8.0):
        """Send Lua on channel 0, return the first channel-0 text reply."""
        self.send(0, code.encode())
        for ch, p in self.frames(timeout):
            if ch == 0 and p[:1] != b"\x01":
                return p.decode(errors="replace").strip()
        return None

    def close(self):
        self.sock.close()


def check(name, ok, detail=""):
    print(f"repl_smoke: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def wait_for_repl(port, timeout, proc=None):
    """Block until the Lua runtime answers on `port`, or give up.

    Booting to the Lua runtime takes ~10 s of guest time here and rather
    more on a slow CI runner, so poll for the runtime instead of sleeping
    a fixed amount: the fast case costs nothing and the slow case does
    not fail spuriously.  The bridge accepts TCP before the guest is up,
    so a connect alone proves nothing — evaluate something.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc is not None and proc.poll() is not None:
            sys.exit(f"repl_smoke: halo-emu exited early "
                     f"({proc.returncode})")
        try:
            r = Repl(port, timeout=5)
        except OSError:
            time.sleep(1.0)
            continue
        try:
            r.send(0, b"\x03")  # interrupt whatever main.lua is doing
            time.sleep(0.5)
            r.drain()
            if r.lua("print('up')", timeout=5) == "up":
                return r
        except OSError:
            pass
        r.close()
        time.sleep(1.0)
    sys.exit(f"repl_smoke: Lua runtime did not answer within {timeout:g}s")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"),
                   help="firmware image (default: ./0.8.8.bin)")
    p.add_argument("--port", type=int, default=9563)
    p.add_argument("--boot-wait", type=float, default=120.0,
                   help="seconds to wait for the Lua runtime to answer "
                        "(default 120; the probe returns as soon as it "
                        "does, so a fast host is not slowed down)")
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"repl_smoke: firmware not found: {args.firmware}")

    flash = tempfile.mktemp(prefix="halo-repl-smoke-", suffix=".img")
    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware, "--flash", flash, "--headless",
         "--repl-port", str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    try:
        r = wait_for_repl(args.port, args.boot_wait, proc)

        # 1. REPL echo
        out = r.lua("print('echo-check')")
        check("REPL echo", out == "echo-check", repr(out))

        # 2. MTU
        out = r.lua("print(frame.bluetooth.max_length())")
        check("max_length() == 511", out == "511", repr(out))

        # 3. data channel, both directions
        r.lua("frame.bluetooth.receive_callback("
              "function(d) frame.bluetooth.send('len=' .. #d) end) "
              "print('cb-set')")
        r.send(0, b"\x01" + b"D" * 100)
        reply = None
        for ch, pay in r.frames():
            if ch == 0 and pay[:1] == b"\x01":
                reply = pay[1:]
                break
        check("data channel round-trip", reply == b"len=100", repr(reply))

        # 4. single-client policy
        s2 = socket.create_connection(("127.0.0.1", args.port), timeout=5)
        s2.settimeout(5)
        try:
            refused = s2.recv(16) == b""
        except (socket.timeout, ConnectionResetError):
            refused = False
        s2.close()
        check("second client refused", refused)

        # 5a. 0x03 interrupts a busy loop
        r.send(0, b"while true do end")
        time.sleep(0.5)
        r.send(0, b"\x03")
        time.sleep(0.5)
        r.drain()
        out = r.lua("print('post-interrupt')")
        check("0x03 interrupt", out == "post-interrupt", repr(out))

        # 5b. 0x04 restarts the VM
        r.send(0, b"\x04")
        time.sleep(1.0)
        r.drain()
        out = r.lua("print('post-restart')")
        check("0x04 VM restart", out == "post-restart", repr(out))

        # 5c. 0x02 reboots with /lfs intact
        out = r.lua("f=frame.file.open('/smoke-reboot.txt','w') "
                    "f:write('persist-me') f:close() print('written')")
        check("write /lfs before reboot", out == "written", repr(out))
        r.send(0, b"\x02")
        dropped = any(ch == "EOF" for ch, _ in r.frames(30))
        check("reboot drops the client", dropped)
        r.close()

        deadline = time.monotonic() + 40
        r = None
        while time.monotonic() < deadline:
            time.sleep(2)
            try:
                r = Repl(args.port, timeout=5)
                r.send(0, b"\x03")
                time.sleep(0.5)
                r.drain()
                out = r.lua("print('up')", timeout=5)
                if out == "up":
                    break
                r.close()
                r = None
            except OSError:
                r = None
        check("REPL reachable after reboot", r is not None)
        out = r.lua("f=frame.file.open('/smoke-reboot.txt','r') "
                    "print(f:read()) f:close()")
        check("/lfs persisted across reboot", out == "persist-me",
              repr(out))

        print("repl_smoke: PASS — all checks green")
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
            print("repl_smoke: FAIL", file=sys.stderr)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
