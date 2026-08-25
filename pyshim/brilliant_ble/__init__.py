"""brilliant_ble — emulator shim of the Brilliant Labs phone library
(ticket 0030).

Drop-in replacement for the PyPI `brilliant-ble` package (API of 3.2.x)
that redirects every BLE operation to the halo-emu Lua REPL bridge on
tcp://127.0.0.1:9563, so the upstream device test-suite
(applications/halo/tests/) runs unmodified against the emulator:

    PYTHONPATH=<emulator-repo>/pyshim python3 tests/test_version.py

The bridge speaks the retired native_sim wire protocol —
[u8 channel][u16 LE length][payload]; channel 0 carries the exact Lua
RX/TX GATT PDUs (REPL text, 0x01-marked data, control codes), channel 1
audio, channel 2 video.  Address override: HALO_EMU_ADDR=host:port.

Not implemented: OTA (ota_flash_firmware / ota_confirm raise OtaError)
and real scanning (connect() returns immediately; a requested `name` is
echoed back).
"""

import asyncio
import os
import socket
import struct
from enum import Enum
from typing import Final, List

__all__ = ["BrilliantBle", "BrilliantDeviceType", "OtaError",
           "chunk_lua_string"]

_MTU = 512  # usable ATT payload the emulator reports (max_length 511)


class OtaError(Exception):
    pass


class BrilliantDeviceType(Enum):
    FRAME = "Frame"
    HALO = "Halo"
    UNKNOWN = "Unknown"


def _emu_addr():
    addr = os.environ.get("HALO_EMU_ADDR", "127.0.0.1:9563")
    host, _, port = addr.rpartition(":")
    return host or "127.0.0.1", int(port)


def emu_control(*commands, timeout=90.0):
    """Emulator-only escape hatch (not part of the real phone library):
    send text verbs to halo-emu's control socket (ticket 0031) — e.g.
    emu_control("button click"), emu_control("battery set 82%") — and
    return the reply lines.  Address: $HALO_EMU_CTL (default
    127.0.0.1:9562, run_emu_tests.py exports it).  Raises RuntimeError
    on an `err` reply.  The generous default timeout covers `button
    hold <ms>` verbs, which reply only after the release."""
    import socket

    addr = os.environ.get("HALO_EMU_CTL", "127.0.0.1:9562")
    host, _, port = addr.rpartition(":")
    replies = []
    with socket.create_connection((host or "127.0.0.1", int(port)),
                                  timeout=timeout) as sock:
        f = sock.makefile("rw", encoding="utf-8", newline="\n")
        for cmd in commands:
            f.write(cmd.strip() + "\n")
            f.flush()
            reply = f.readline().strip()
            if not reply.startswith("ok"):
                raise RuntimeError(f"emu_control({cmd!r}): {reply}")
            replies.append(reply)
    return replies


def chunk_lua_string(payload: bytes, max_chunk_bytes: int) -> List[str]:
    """Split escaped-Lua-string bytes into chunks that never cut a
    multi-byte UTF-8 sequence or a Lua escape (verbatim semantics of the
    real package)."""
    if max_chunk_bytes <= 0:
        raise ValueError("max_chunk_bytes must be positive")

    chunks: List[str] = []
    index = 0
    while index < len(payload):
        end = index + max_chunk_bytes
        if end >= len(payload):
            end = len(payload)
        else:
            while end > index and (payload[end] & 0xC0) == 0x80:
                end -= 1
            trailing = 0
            while (end - 1 - trailing >= index
                   and payload[end - 1 - trailing] == 0x5C):
                trailing += 1
            if trailing % 2 == 1:
                end -= 1
            if end == index:
                raise ValueError("max_chunk_bytes is too small to hold "
                                 "the next character of the payload")
        chunks.append(payload[index:end].decode())
        index = end
    return chunks


class BrilliantBle:
    """API-compatible stand-in for brilliant_ble.BrilliantBle."""

    def __init__(self):
        self._reader = None
        self._writer = None
        self._name = None
        self._type = BrilliantDeviceType.UNKNOWN
        self._rx_task = None
        self._print_response = asyncio.Queue()
        self._data_response = asyncio.Queue()
        self._awaiting_print_response = False
        self._awaiting_data_response = False
        self._user_print_response_handler = lambda _: None
        self._user_data_response_handler = lambda _: None
        self._user_disconnect_handler = lambda: None

    # ------------------------------------------------------------- #
    # Transport                                                      #
    # ------------------------------------------------------------- #

    async def _rx_loop(self):
        try:
            while True:
                hdr = await self._reader.readexactly(3)
                channel, ln = struct.unpack("<BH", hdr)
                payload = (await self._reader.readexactly(ln)) if ln \
                    else b""
                if channel == 0:
                    await self._dispatch_lua(payload)
                # channel 1 (audio) / 2 (video): no test consumes them
                # through this class API today; ignore.
        except (asyncio.IncompleteReadError, ConnectionResetError,
                OSError):
            pass
        finally:
            self._user_disconnect_handler()
            self.__init__()

    async def _dispatch_lua(self, data):
        if data[:1] == b"\x01":
            view = memoryview(data)[1:]
            if self._awaiting_data_response:
                self._awaiting_data_response = False
                await self._data_response.put(view)
            handler = self._user_data_response_handler
            if handler is not None:
                if asyncio.iscoroutinefunction(handler):
                    await handler(view)
                else:
                    handler(view)
        else:
            decoded = data.decode()
            if self._awaiting_print_response:
                self._awaiting_print_response = False
                await self._print_response.put(decoded)
            handler = self._user_print_response_handler
            if handler is not None:
                if asyncio.iscoroutinefunction(handler):
                    await handler(decoded)
                else:
                    handler(decoded)

    async def _send_frame(self, channel, payload):
        if self._writer is None:
            raise Exception("Not connected")
        self._writer.write(struct.pack("<BH", channel, len(payload)) +
                           bytes(payload))
        await self._writer.drain()

    # ------------------------------------------------------------- #
    # Public API (mirrors brilliant_ble 3.2.x)                       #
    # ------------------------------------------------------------- #

    @property
    def type(self):
        return self._type

    @property
    def name(self):
        return self._name

    async def connect(self, name=None, timeout=10,
                      print_response_handler=lambda _: None,
                      data_response_handler=lambda _: None,
                      disconnect_handler=lambda: None):
        self._user_print_response_handler = print_response_handler
        self._user_data_response_handler = data_response_handler
        self._user_disconnect_handler = disconnect_handler

        host, port = _emu_addr()
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=timeout)
        except (OSError, asyncio.TimeoutError) as e:
            raise Exception(f"Error connecting: {e}")

        self._type = BrilliantDeviceType.HALO
        self._name = name or os.environ.get("HALO_EMU_NAME", "Halo EMU")
        self._rx_task = asyncio.ensure_future(self._rx_loop())
        # the bridge injects CONNECT/pairing on attach; give it a moment
        await asyncio.sleep(0.3)
        return self._name

    async def disconnect(self):
        if self._writer is not None:
            self._writer.close()
        if self._rx_task is not None:
            try:
                await asyncio.wait_for(self._rx_task, timeout=2)
            except (asyncio.TimeoutError, Exception):
                pass

    def is_connected(self):
        return self._writer is not None and not self._writer.is_closing()

    def max_lua_payload(self):
        return min(_MTU, 512) - 3 if self.is_connected() else 0

    def max_data_payload(self):
        return min(_MTU, 512) - 4 if self.is_connected() else 0

    async def _transmit(self, data, show_me=False, await_bt_response=True):
        if show_me:
            print(data)
        if len(data) > _MTU - 3:
            raise Exception("payload length is too large")
        await self._send_frame(0, data)

    async def send_lua(self, string: str, show_me=False, await_print=False,
                       timeout=5):
        self._awaiting_print_response = await_print
        await self._transmit(string.encode(), show_me=show_me)
        if await_print:
            try:
                return await asyncio.wait_for(self._print_response.get(),
                                              timeout=timeout)
            except asyncio.TimeoutError:
                raise Exception("device didn't respond")

    async def send_data(self, data, show_me=False, await_data=False,
                        timeout=5, await_bt_response=True):
        self._awaiting_data_response = await_data
        await self._transmit(bytearray(b"\x01") + data, show_me=show_me,
                             await_bt_response=await_bt_response)
        if await_data:
            try:
                return await asyncio.wait_for(self._data_response.get(),
                                              timeout=timeout)
            except asyncio.TimeoutError:
                raise Exception("device didn't respond")

    async def send_audio(self, data, await_bt_response=False):
        if len(data) > _MTU - 3:
            return  # silently dropped, like the real library
        await self._send_frame(1, data)

    async def drain_print_channel(self, quiet=0.25, max_total=1.5):
        loop = asyncio.get_running_loop()
        start = loop.time()
        while loop.time() - start < max_total:
            self._awaiting_print_response = True
            try:
                await asyncio.wait_for(self._print_response.get(),
                                       timeout=quiet)
            except asyncio.TimeoutError:
                break
        self._awaiting_print_response = False

    async def send_reset_signal(self, show_me=False):
        await self._transmit(bytearray(b"\x04"), show_me=show_me)
        await asyncio.sleep(0.2)

    async def send_remove_signal(self, show_me=False):
        await self._transmit(bytearray(b"\x05"), show_me=show_me)
        await asyncio.sleep(0.2)

    async def send_break_signal(self, show_me=False):
        await self._transmit(bytearray(b"\x03"), show_me=show_me)
        await asyncio.sleep(0.2)

    async def upload_file_from_string(self, content: str,
                                      frame_file_path="main.lua"):
        content = (content.replace("\r", "")
                          .replace("\\", "\\\\")
                          .replace("\n", "\\n")
                          .replace("\t", "\\t")
                          .replace("'", "\\'")
                          .replace('"', '\\"'))
        await self.send_lua(
            f"f=frame.file.open('{frame_file_path}','w');print(1)",
            await_print=True)
        for chunk in chunk_lua_string(content.encode(),
                                      self.max_lua_payload() - 22):
            await self.send_lua(f'f:write("{chunk}");print(1)',
                                await_print=True)
        await self.send_lua("f:close();print(nil)", await_print=True)

    async def upload_file(self, local_file_path: str,
                          frame_file_path="main.lua"):
        if not os.path.exists(local_file_path):
            raise FileNotFoundError(
                f"Local file not found: {local_file_path}")
        with open(local_file_path, "r") as f:
            content = f.read()
        await self.upload_file_from_string(content, frame_file_path)

    async def ota_flash_firmware(self, firmware, progress_handler=None,
                                 confirm=True, reboot=True,
                                 chunk_size=384):
        raise OtaError("OTA is not supported by the emulator shim")

    async def ota_confirm(self):
        raise OtaError("OTA is not supported by the emulator shim")

    async def send_message(self, msg_code: int, payload: bytes,
                           show_me: bool = False) -> None:
        HEADER_SIZE: Final = 3
        SUBSEQUENT_HEADER_SIZE: Final = 1
        MAX_TOTAL_SIZE: Final = 65535

        if not 0 <= msg_code <= 255:
            raise ValueError(f"Message code must be 0-255, got {msg_code}")
        if self.is_connected() is False:
            raise ValueError(
                "Cannot send message: Not connected to any device")
        total_size = len(payload)
        if total_size > MAX_TOTAL_SIZE:
            raise ValueError(f"Payload size {total_size} exceeds maximum "
                             f"{MAX_TOTAL_SIZE} bytes")

        max_first_chunk = self.max_data_payload() - HEADER_SIZE
        max_chunk_size = self.max_data_payload() - SUBSEQUENT_HEADER_SIZE
        buffer = bytearray(self.max_data_payload())

        first_chunk_size = min(max_first_chunk, total_size)
        buffer[0] = msg_code
        buffer[1] = total_size >> 8
        buffer[2] = total_size & 0xFF
        buffer[HEADER_SIZE:HEADER_SIZE + first_chunk_size] = \
            payload[:first_chunk_size]
        await self.send_data(
            memoryview(buffer)[:HEADER_SIZE + first_chunk_size],
            show_me=show_me, await_data=True)
        sent = first_chunk_size

        if sent < total_size:
            buffer[0] = msg_code
            while sent < total_size:
                chunk = min(max_chunk_size, total_size - sent)
                buffer[SUBSEQUENT_HEADER_SIZE:
                       SUBSEQUENT_HEADER_SIZE + chunk] = \
                    payload[sent:sent + chunk]
                await self.send_data(
                    memoryview(buffer)[:SUBSEQUENT_HEADER_SIZE + chunk],
                    show_me=show_me, await_data=True)
                sent += chunk
