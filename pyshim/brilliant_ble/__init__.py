# Copyright (c) 2025 Brilliant Labs
# SPDX-License-Identifier: Apache-2.0
"""Drop-in stand-in for the `brilliant-ble` PyPI package (ticket 0006).

Lets the unmodified device tests in `applications/halo/tests/` drive the
desktop emulator instead of a real Halo over BLE: put this package's parent
directory (`emulator/pyshim`) on PYTHONPATH and it shadows the real package,
speaking the ticket-0005 TCP framing to a running `zephyr.exe`:

    [u8 channel][u16 little-endian length][payload]

to HALO_EMU_ADDR (default `127.0.0.1:9563`). Channel 0 carries the exact
GATT PDU a real device would see on the Lua RX/TX characteristics (0x01-marked
data, 0x02..0x07 control codes, anything else REPL text); channel 1 is audio,
channel 2 video (tickets 0014/0016).

Activation (no test-file edits):

    PYTHONPATH=emulator/pyshim HALO_EMU_ADDR=127.0.0.1:9563 \
        python3 applications/halo/tests/test_version.py --name "Halo 00"

The API below mirrors what the tests actually use, including the private
`_user_print_response_handler` attribute they swap out directly
(halo_device_file._Fenced, tools/verify.py). Semantics preserved from
hardware: `await_print`/`await_data` hand back only the FIRST notification
of a reply; the data handler receives a buffer with no `.decode()`
(a bytearray here); print payloads arrive without a trailing newline
(luaport.h's `lua_writeline()` is empty).
"""

import asyncio
import os
import struct
import traceback

_CH_LUA = 0
_CH_AUDIO = 1
_CH_VIDEO = 2

_DATA_MARKER = 0x01
_CTRL_REBOOT = 0x02      # Ctrl-B: reboot (emulator: process exits)
_CTRL_INTERRUPT = 0x03   # Ctrl-C: break script execution
_CTRL_RESTART = 0x04     # Ctrl-D: restart the Lua VM (re-runs main.lua)
_CTRL_RESET = 0x05       # Ctrl-E: remove main.lua and restart
_CTRL_EXIT = 0x06        # Ctrl-F: exit the Lua runtime completely

_DEFAULT_ADDR = "127.0.0.1:9563"
_REPLY_TIMEOUT = 10.0    # seconds to wait on await_print / await_data
_CONNECT_TIMEOUT = 30.0  # seconds to keep retrying the initial TCP connect
_CTRL_SETTLE = 0.2       # seconds for a control code to take effect


def _emu_addr():
    addr = os.environ.get("HALO_EMU_ADDR", _DEFAULT_ADDR)
    host, sep, port = addr.rpartition(":")
    if not sep:
        host, port = addr, "9563"
    return host or "127.0.0.1", int(port)


class BrilliantBle:
    """API-compatible replacement for brilliant_ble.BrilliantBle."""

    def __init__(self):
        self._reader = None
        self._writer = None
        self._rx_task = None
        # Tests reach into these directly; they must exist from construction
        # and be swappable at any time (see halo_device_file._Fenced.probe).
        self._user_print_response_handler = None
        self._user_data_response_handler = None
        self._user_audio_response_handler = None
        self._user_video_response_handler = None
        self._user_disconnect_handler = None
        self._awaiting_print = False
        self._print_response = None
        self._print_event = asyncio.Event()
        self._awaiting_data = False
        self._data_response = None
        self._data_event = asyncio.Event()
        # Mirrors the emulator's CONFIG_HALO_EMU_MTU (the negotiated ATT MTU
        # on hardware); payload maxima below derive from it exactly like the
        # real package derives them from bleak's mtu_size.
        self._mtu = int(os.environ.get("HALO_EMU_MTU", "512"))

    # -- connection -------------------------------------------------------

    async def connect(
        self,
        name=None,
        print_response_handler=None,
        data_response_handler=None,
        audio_response_handler=None,
        video_response_handler=None,
        disconnect_handler=None,
    ):
        """Connect to the emulator's TCP transport; returns the device name.

        `name` is accepted for signature compatibility (tests pass their
        --name argument through) but there is nothing to scan for: the
        address comes from HALO_EMU_ADDR. Retries until the emulator's
        listener is up, so a just-launched zephyr.exe works.
        """
        if print_response_handler is not None:
            self._user_print_response_handler = print_response_handler
        if data_response_handler is not None:
            self._user_data_response_handler = data_response_handler
        if audio_response_handler is not None:
            self._user_audio_response_handler = audio_response_handler
        if video_response_handler is not None:
            self._user_video_response_handler = video_response_handler
        if disconnect_handler is not None:
            self._user_disconnect_handler = disconnect_handler

        host, port = _emu_addr()
        loop = asyncio.get_running_loop()
        deadline = loop.time() + _CONNECT_TIMEOUT
        while True:
            try:
                self._reader, self._writer = await asyncio.open_connection(host, port)
                break
            except OSError:
                if loop.time() >= deadline:
                    raise Exception(
                        f"could not reach the emulator at {host}:{port} "
                        f"within {_CONNECT_TIMEOUT:.0f}s — is zephyr.exe running?"
                    )
                await asyncio.sleep(0.2)

        self._rx_task = asyncio.create_task(self._rx_loop())
        # The emulator's fixed EUI-48 is 2C:F7:F1:00:00:01; per PROTOCOL.md
        # the advertised name is "Halo XX" with XX = the 4th byte in hex.
        return name if name else "Halo 00"

    async def disconnect(self):
        if self._rx_task is not None:
            self._rx_task.cancel()
            try:
                await self._rx_task
            except (asyncio.CancelledError, Exception):
                pass
            self._rx_task = None
        if self._writer is not None:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except (ConnectionError, OSError):
                pass
            self._writer = None
        self._reader = None

    def is_connected(self):
        return self._writer is not None and not self._writer.is_closing()

    # -- payload limits ---------------------------------------------------

    def max_lua_payload(self):
        """Largest Lua string send_lua() accepts (MTU - 3, as on hardware)."""
        return self._mtu - 3

    def max_data_payload(self):
        """Largest send_data() payload (MTU - 4: one more for the 0x01 marker)."""
        return self._mtu - 4

    # -- transmit paths ---------------------------------------------------

    async def _transmit(self, channel, payload, show_me=False):
        if show_me:
            print(payload)
        if not self.is_connected():
            raise Exception("Not connected to device")
        self._writer.write(struct.pack("<BH", channel, len(payload)) + payload)
        await self._writer.drain()

    async def _wait_reply(self, event, what):
        try:
            await asyncio.wait_for(event.wait(), timeout=_REPLY_TIMEOUT)
        except asyncio.TimeoutError:
            raise Exception(f"device did not reply with {what} "
                            f"within {_REPLY_TIMEOUT:.0f}s")

    async def send_lua(self, string, show_me=False, await_print=False):
        """Send one REPL command; a newline terminates a command, so `string`
        must be a one-liner. With await_print, returns the FIRST print
        notification that follows (longer replies bleed into the next
        command's answer — exactly as on hardware; fence if you need more)."""
        data = string.encode()
        if len(data) > self.max_lua_payload():
            raise Exception(f"payload of {len(data)} bytes exceeds "
                            f"max_lua_payload() of {self.max_lua_payload()}")
        if await_print:
            self._print_event.clear()
            self._print_response = None
            self._awaiting_print = True
        await self._transmit(_CH_LUA, data, show_me)
        if await_print:
            try:
                await self._wait_reply(self._print_event, "a print response")
            finally:
                self._awaiting_print = False
            return self._print_response

    async def send_data(self, data, show_me=False, await_data=False):
        """Send user data (0x01-marked PDU on the Lua channel). With
        await_data, returns the first data notification that follows."""
        data = bytes(data)
        if len(data) > self.max_data_payload():
            raise Exception(f"payload of {len(data)} bytes exceeds "
                            f"max_data_payload() of {self.max_data_payload()}")
        if await_data:
            self._data_event.clear()
            self._data_response = None
            self._awaiting_data = True
        await self._transmit(_CH_LUA, bytes([_DATA_MARKER]) + data, show_me)
        if await_data:
            try:
                await self._wait_reply(self._data_event, "a data response")
            finally:
                self._awaiting_data = False
            return self._data_response

    async def send_audio(self, data, show_me=False):
        """Stream one audio PDU (speaker path, TCP channel 1). Anything larger
        than the MTU allows is silently dropped, like the hardware package."""
        data = bytes(data)
        if len(data) > self.max_lua_payload():
            return
        await self._transmit(_CH_AUDIO, data, show_me)

    # -- control signals --------------------------------------------------

    async def _send_ctrl(self, code):
        await self._transmit(_CH_LUA, bytes([code]))
        # give the runtime a moment to act before the next command lands
        await asyncio.sleep(_CTRL_SETTLE)

    async def send_break_signal(self, show_me=False):
        """Ctrl-C: interrupt whatever the Lua VM is executing."""
        await self._send_ctrl(_CTRL_INTERRUPT)

    async def send_reset_signal(self, show_me=False):
        """Ctrl-D: restart the Lua VM (main.lua runs again)."""
        await self._send_ctrl(_CTRL_RESTART)

    async def send_remove_signal(self, show_me=False):
        """Ctrl-E: delete main.lua and restart the VM."""
        await self._send_ctrl(_CTRL_RESET)

    async def send_exit_signal(self, show_me=False):
        """Ctrl-F: exit the Lua runtime completely."""
        await self._send_ctrl(_CTRL_EXIT)

    async def send_reboot_signal(self, show_me=False):
        """Ctrl-B: reboot — the emulator process exits (relaunch = reboot)."""
        await self._send_ctrl(_CTRL_REBOOT)

    # -- file upload ------------------------------------------------------

    async def upload_file_from_string(self, string, frame_file_path="main.lua"):
        """Write `string` to a file on the device via the REPL file API.

        Chunked to the Lua payload limit with every byte escaped so content
        survives byte for byte; each chunk is fenced with an awaited print so
        the REPL consumes them strictly in order and at its own pace.
        """
        pieces = [self._escape_lua_char(c) for c in string]
        # room for: f:write("")print(0)  around the escaped chunk
        budget = self.max_lua_payload() - len('f:write("")print(0)')

        await self.send_lua(
            f'f=frame.file.open("{frame_file_path}","w")print(0)',
            await_print=True,
        )
        chunk = ""
        for piece in pieces:
            if len(chunk) + len(piece) > budget:
                await self.send_lua(f'f:write("{chunk}")print(0)', await_print=True)
                chunk = ""
            chunk += piece
        if chunk:
            await self.send_lua(f'f:write("{chunk}")print(0)', await_print=True)
        await self.send_lua("f:close()print(0)", await_print=True)

    async def upload_file(self, path, frame_file_path="main.lua"):
        with open(path, "r") as f:
            await self.upload_file_from_string(f.read(), frame_file_path)

    @staticmethod
    def _escape_lua_char(c):
        if c == "\\":
            return "\\\\"
        if c == '"':
            return '\\"'
        if c == "'":
            return "\\'"
        if c == "\n":
            return "\\n"
        if c == "\r":
            return "\\r"
        if c == "\t":
            return "\\t"
        if " " <= c <= "~":
            return c
        # anything else (control chars, non-ASCII) as padded decimal escapes,
        # one per UTF-8 byte — \ddd must be 3 digits or a following digit
        # would extend the escape
        return "".join(f"\\{b:03d}" for b in c.encode())

    # -- receive path -----------------------------------------------------

    async def _rx_loop(self):
        try:
            while True:
                header = await self._reader.readexactly(3)
                channel, length = struct.unpack("<BH", header)
                payload = await self._reader.readexactly(length) if length else b""
                try:
                    self._dispatch(channel, payload)
                except Exception:
                    # a broken user handler must not wedge the transport,
                    # but the failure has to be visible in the test output
                    traceback.print_exc()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            self._writer = None
            self._reader = None
            if self._user_disconnect_handler:
                self._user_disconnect_handler()

    def _dispatch(self, channel, payload):
        if channel == _CH_LUA:
            if payload[:1] == bytes([_DATA_MARKER]) and len(payload) > 1:
                data = bytearray(payload[1:])
                if self._awaiting_data:
                    self._awaiting_data = False
                    self._data_response = data
                    self._data_event.set()
                if self._user_data_response_handler:
                    self._user_data_response_handler(data)
            else:
                text = payload.decode("utf-8", errors="replace")
                if self._awaiting_print:
                    self._awaiting_print = False
                    self._print_response = text
                    self._print_event.set()
                if self._user_print_response_handler:
                    self._user_print_response_handler(text)
        elif channel == _CH_AUDIO:
            if self._user_audio_response_handler:
                self._user_audio_response_handler(bytearray(payload))
        elif channel == _CH_VIDEO:
            if self._user_video_response_handler:
                self._user_video_response_handler(bytearray(payload))
