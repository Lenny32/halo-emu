#!/usr/bin/env python3
# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0
"""Ticket 0005 acceptance smoke test for the emulator's TCP Lua transport.

Speaks the framed wire protocol documented in
emulator/module/include/halo/lua_transport.h:

    [u8 channel][u16 little-endian length][payload]

against a running emulator (default 127.0.0.1:9563) and checks the REPL,
data channel, MTU reporting, single-client policy and the 0x03/0x04/0x06
control codes. `--reboot` additionally sends 0x02, after which the emulator
process exits (relaunch = reboot), so it comes last and is opt-in.

Usage:
    emulator/build.sh
    emulator/build/halo-emu &
    emulator/tools/repl_smoke.py [--port 9563] [--reboot]
"""
import argparse
import socket
import struct
import sys
import time

CH_LUA = 0


def frame(ch, payload):
    return struct.pack("<BH", ch, len(payload)) + payload


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def send_lua(self, text):
        self.s.sendall(frame(CH_LUA, text.encode()))

    def send_ctrl(self, code):
        self.s.sendall(frame(CH_LUA, bytes([code])))

    def recv_frames(self, timeout=3.0):
        """Collect whole frames until quiet for 0.3 s or timeout."""
        self.s.settimeout(0.3)
        out = []
        end = time.time() + timeout
        while time.time() < end:
            try:
                data = self.s.recv(4096)
                if not data:
                    break
                self.buf += data
            except socket.timeout:
                if out:
                    break
                continue
            while len(self.buf) >= 3:
                ch, ln = struct.unpack("<BH", self.buf[:3])
                if len(self.buf) < 3 + ln:
                    break
                out.append((ch, self.buf[3:3 + ln]))
                self.buf = self.buf[3 + ln:]
        return out

    def repl_text(self, timeout=3.0):
        return b"".join(p for ch, p in self.recv_frames(timeout) if ch == CH_LUA)


failed = False


def check(name, cond, detail=""):
    global failed
    print(f"  {'OK  ' if cond else 'FAIL'} {name} {detail}")
    failed = failed or not cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9563)
    ap.add_argument("--reboot", action="store_true",
                    help="also test 0x02 (the emulator process exits)")
    args = ap.parse_args()

    c = Client(args.port)
    time.sleep(0.3)

    c.send_lua("print('hi')")
    out = c.repl_text()
    check("print('hi') echoes", b"hi" in out, repr(out[:80]))

    c.send_lua("print(frame.bluetooth.max_length())")
    out = c.repl_text()
    check("max_length() == 511", b"511" in out, repr(out[:80]))

    c.send_lua("print(frame.bluetooth.is_connected())")
    out = c.repl_text()
    check("is_connected() true", b"true" in out, repr(out[:80]))

    c.send_lua("print(frame.bluetooth.address())")
    out = c.repl_text()
    check("address fixed EUI", b"2C:F7:F1:00:00:01" in out, repr(out[:80]))

    c.send_lua("frame.bluetooth.send('DATAPDU')")
    frames = c.recv_frames()
    data_pdus = [p for ch, p in frames if ch == CH_LUA and p[:1] == b"\x01"]
    check("data send round-trips with 0x01 marker",
          any(b"DATAPDU" in p for p in data_pdus), repr(frames[:3]))

    c.send_lua("frame.bluetooth.send(string.rep('x', 600)) print('sent600')")
    frames = c.recv_frames()
    total = sum(len(p) - 1 for ch, p in frames if ch == CH_LUA and p[:1] == b"\x01")
    ok_echo = any(b"sent600" in p for ch, p in frames if ch == CH_LUA)
    check("oversize send chunked (600 bytes, no error)", total == 600 and ok_echo,
          f"marked payload bytes={total}")

    # Single-client policy: a second connection is accepted and immediately
    # closed by the emulator.
    c2 = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    c2.settimeout(2)
    try:
        refused = c2.recv(64) == b""
    except ConnectionResetError:
        refused = True
    except socket.timeout:
        try:
            c2.sendall(frame(CH_LUA, b"print('intruder')"))
            refused = c2.recv(64) == b""
        except (BrokenPipeError, ConnectionResetError):
            refused = True
    c2.close()
    check("second client refused", refused)

    c.send_lua("while true do end")
    time.sleep(1.0)
    c.send_ctrl(0x03)
    time.sleep(0.5)
    c.send_lua("print('alive-after-interrupt')")
    out = c.repl_text(5.0)
    check("0x03 interrupts busy loop", b"alive-after-interrupt" in out, repr(out[:120]))

    c.send_lua("marker_var = 42")
    c.repl_text(1.0)
    c.send_ctrl(0x04)
    time.sleep(1.0)
    c.send_lua("print(tostring(marker_var))")
    out = c.repl_text(5.0)
    check("0x04 restarts VM (globals wiped)", b"nil" in out, repr(out[:120]))

    c.send_ctrl(0x06)
    time.sleep(1.0)
    c.send_lua("print('post-exit')")
    out = c.repl_text(2.0)
    check("0x06 exits runtime (no echo)", b"post-exit" not in out, repr(out[:80]))

    if args.reboot:
        c.send_ctrl(0x02)
        time.sleep(1.5)
        try:
            c.s.settimeout(2)
            gone = c.s.recv(16) == b""
        except (ConnectionResetError, BrokenPipeError):
            gone = True
        except socket.timeout:
            gone = False
        check("0x02 reboot drops the connection (process exited)", gone)

    print("FAILED" if failed else "ALL PASS")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
