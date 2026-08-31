#!/usr/bin/env python3
"""run_app.py — install and start a Lua application on a running halo-emu.

Acts as the phone: connects to the Lua REPL bridge (tcp://127.0.0.1:9563),
stops whatever is running, writes the given file to /lfs/main.lua and
restarts the Lua VM, which auto-runs main.lua — exactly the flow the real
phone app uses.  The app persists in mram.img, so the emulator boots
straight into it from then on.

    # terminal 1: the device
    ./halo-emu -f 0.8.8.bin

    # terminal 2: install an app
    python3 tools/run_app.py examples/hello.lua

Options:
    --watch     stay connected and live-print the app's print() output
                (Ctrl-C to detach; the app keeps running)
    --remove    uninstall: delete main.lua and restart into the idle REPL

Address override: HALO_EMU_ADDR=host:port (default 127.0.0.1:9563).
"""

import argparse
import asyncio
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "pyshim"))
from brilliant_ble import BrilliantBle  # noqa: E402


async def main():
    ap = argparse.ArgumentParser(
        description="Install and start a Lua app on a running halo-emu.")
    ap.add_argument("app", nargs="?", help="Lua file to install as main.lua")
    ap.add_argument("--watch", action="store_true",
                    help="stay connected and print the app's print() output")
    ap.add_argument("--remove", action="store_true",
                    help="delete main.lua and restart into the idle REPL")
    args = ap.parse_args()
    if bool(args.app) == args.remove:
        ap.error("give a Lua file to install, or --remove to uninstall")

    b = BrilliantBle()
    try:
        await b.connect()
    except OSError as e:
        sys.exit(f"cannot reach the emulator's REPL bridge ({e}) — "
                 "is halo-emu running?")

    # A main.lua busy-loop holds the Lua VM, so break first to get a prompt.
    await b.send_break_signal()
    await b.drain_print_channel()

    if args.remove:
        await b.send_remove_signal()   # firmware deletes main.lua + resets VM
        print("main.lua removed; device is back at the idle REPL")
        await b.disconnect()
        return

    await b.upload_file(args.app, "main.lua")
    await b.send_reset_signal()        # restart the Lua VM -> main.lua runs
    print(f"{args.app} installed as main.lua and started "
          "(persists across reboots)")

    if args.watch:
        b._user_print_response_handler = lambda s: print(s, end="",
                                                         flush=True)
        print("--- watching app output, Ctrl-C to detach ---")
        try:
            while b.is_connected():
                await asyncio.sleep(0.5)
        except KeyboardInterrupt:
            pass
    await b.disconnect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
