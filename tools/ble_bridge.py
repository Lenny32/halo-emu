"""ble_bridge — Lua REPL bridge between a TCP client and the halo-ble
doorbell device (ticket 0030).

Serves the retired native_sim emulator's wire protocol on
tcp://127.0.0.1:<port> (default 9563):

    [u8 channel][u16 LE length][payload]

    channel 0 = Lua RX/TX  (payload = the exact GATT PDU: REPL text,
                            0x01-marked data, or a 0x02..0x07 control code)
    channel 1 = audio      (host->guest: Audio RX writes; guest->host:
                            Audio TX notifications)
    channel 2 = video      (guest->host only: Video notifications)

and translates it to the 0028 doorbell framing ({op, flags, len16,
payload}) spoken by the QEMU halo-ble chardev:

    TCP client <-> this bridge <-> doorbell/ring device <-> synthetic ROM
    GATT <-> ble_lua.c characteristics <-> Lua runtime

The bridge is the doorbell chardev's single client.  On firmware boot the
ROM stub dumps the GATT database (EVT_SVC/EVT_ATT); the Lua service
characteristic handles are resolved from that dump by UUID.  When a REPL
client connects the bridge injects OP_CONNECT (the stub then fakes
pairing + encryption, which ble_lua.c requires before accepting writes)
and enables notifications on TX/Video/AudioTX by writing their CCCs.

Policies (matching the native_sim transport):
  - loopback only, one client at a time (a second connection is refused);
  - payloads over the 512-byte MTU are rejected (client dropped);
  - notifications are never dropped: the client-facing socket write
    blocks, which stops the doorbell reader, which backpressures the
    QEMU device (its chardev write stalls) and, through the full G2H
    ring, the firmware's notify path;
  - a fresh GATT database dump while running means the guest rebooted:
    the current client is dropped and the bridge re-arms for the next
    connection.

Control codes ride channel 0 unmodified and are handled by the firmware
itself (0x02 reboot, 0x03 interrupt, 0x04 restart, 0x05 reset, 0x06 exit,
0x07 remove-all).  0x02 makes the guest perform a cold reset; the machine
reboots in place with /lfs (MRAM) intact and the bridge re-arms.
"""

import os
import socket
import struct
import threading
import time

DEBUG = os.environ.get("HALO_BRIDGE_DEBUG", "") not in ("", "0")

MTU = 512  # usable ATT payload; frame.bluetooth.max_length() == MTU - 1

# halo_rom_ipc.h opcodes
OP_CONNECT = 0x01
OP_DISCONNECT = 0x02
OP_GATT_WRITE = 0x03
EVT_NOTIFY = 0x81
EVT_SVC = 0x82
EVT_ATT = 0x83
EVT_CONNECTED = 0x84
EVT_DISCONNECTED = 0x85
EVT_PAIRED = 0x88

# Lua service 7A230001-5475-A6A4-654C-8431F6AD49C4, little-endian as
# dumped by the ROM stub; byte 12 is the characteristic id (01=svc,
# 02=RX, 03=TX, 04=Video, 05=AudioRX, 06=AudioTX).
LUA_UUID_BASE = bytes([0xC4, 0x49, 0xAD, 0xF6, 0x31, 0x84, 0x4C, 0x65,
                       0xA4, 0xA6, 0x75, 0x54, 0x00, 0x00, 0x23, 0x7A])
CCC_UUID = bytes([0x02, 0x29] + [0] * 14)

CH_LUA = 0
CH_AUDIO = 1
CH_VIDEO = 2


def _lua_char(idx):
    u = bytearray(LUA_UUID_BASE)
    u[12] = idx
    return bytes(u)


def doorbell_frame(op, payload=b""):
    return struct.pack("<BBH", op, 0, len(payload)) + payload


def _hard_close(sock):
    """Close a socket another thread may be blocked in recv() on.
    A bare close() defers the FIN until that recv returns (the kernel
    keeps the fd alive for the in-flight syscall); shutdown() both sends
    the FIN and wakes the blocked thread."""
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()


class ReplBridge(threading.Thread):
    """Runs the bridge; use as a daemon thread inside halo-emu."""

    def __init__(self, doorbell_addr, listen_port, log=None):
        super().__init__(name="repl-bridge", daemon=True)
        self.doorbell_addr = doorbell_addr  # unix path or (host, port)
        self.listen_port = listen_port
        self.log = log or (lambda msg: None)

        self._lock = threading.Lock()       # doorbell socket writes
        self._state_lock = threading.Lock() # attribute db + client slot
        self._doorbell = None
        self._client = None
        self._atts = {}          # hdl -> uuid bytes
        self._svc_hdls = set()   # service start handles seen this boot
        self._handles = None     # dict once the Lua service is resolved
        self._db_ready = threading.Event()
        self._connected_evt = threading.Event()
        self._paired_evt = threading.Event()
        self._stop = False

    # ---------------------------------------------------------------- #
    # Doorbell side                                                     #
    # ---------------------------------------------------------------- #

    def _dial_doorbell(self):
        # QEMU (server=on,wait=on) blocks its own init until we connect,
        # but its listener may not exist yet right after Popen — retry.
        deadline = time.monotonic() + 15
        while True:
            try:
                if isinstance(self.doorbell_addr, tuple):
                    return socket.create_connection(self.doorbell_addr)
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.doorbell_addr)
                return s
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)

    def _doorbell_send(self, op, payload=b""):
        if DEBUG:
            self.log(f"op  0x{op:02x} {payload[:24].hex()}")
        with self._lock:
            self._doorbell.sendall(doorbell_frame(op, payload))

    def _gatt_write(self, hdl, data):
        self._doorbell_send(OP_GATT_WRITE, struct.pack("<H", hdl) + data)

    def _resolve_handles(self):
        """Find the Lua service characteristic value handles and the CCCs
        that immediately follow the notify characteristics."""
        hdls = {}
        for hdl, uuid in sorted(self._atts.items()):
            for name, idx in (("rx", 2), ("tx", 3), ("video", 4),
                              ("audio_rx", 5), ("audio_tx", 6)):
                if uuid == _lua_char(idx):
                    hdls[name] = hdl
        for name in ("tx", "video", "audio_tx"):
            hdl = hdls.get(name)
            if hdl is not None and self._atts.get(hdl + 1) == CCC_UUID:
                hdls[name + "_ccc"] = hdl + 1
        if "rx" in hdls and "tx" in hdls and "tx_ccc" in hdls:
            # Log only when the handles we print actually change: this runs on
            # every EVT_ATT, and the dict keeps growing as the video/audio
            # attributes stream in after rx/tx are already known.
            prev = self._handles
            if prev is None or (prev["rx"], prev["tx"]) != (hdls["rx"],
                                                            hdls["tx"]):
                self.log(f"Lua service resolved: rx=0x{hdls['rx']:04x} "
                         f"tx=0x{hdls['tx']:04x}")
            self._handles = hdls
            self._db_ready.set()

    def _on_guest_reboot(self):
        self.log("guest rebooted (new GATT database dump)")
        with self._state_lock:
            self._atts.clear()
            self._svc_hdls.clear()
            self._handles = None
            self._db_ready.clear()
            self._connected_evt.clear()
            self._paired_evt.clear()
            client, self._client = self._client, None
        if client:
            _hard_close(client)

    def _client_channel_send(self, channel, payload):
        with self._state_lock:
            client = self._client
        if client is None:
            return
        try:
            # Blocking send: a stalled client backpressures the doorbell
            # reader and, through it, the guest's notify path.
            client.sendall(struct.pack("<BH", channel, len(payload)) +
                           payload)
        except OSError:
            pass  # client went away; the client thread cleans up

    def _handle_evt(self, op, payload):
        if DEBUG:
            self.log(f"evt 0x{op:02x} {payload[:24].hex()}")
        if op == EVT_SVC:
            if len(payload) >= 2:
                start_hdl = struct.unpack("<H", payload[:2])[0]
                # The stub allocates handles monotonically from a fixed
                # base; a start handle we have already seen means the
                # guest rebooted and is dumping a fresh database.
                if start_hdl in self._svc_hdls:
                    self._on_guest_reboot()
                self._svc_hdls.add(start_hdl)
            return
        if op == EVT_ATT:
            if len(payload) >= 20:
                hdl, _info = struct.unpack("<HH", payload[:4])
                self._atts[hdl] = payload[4:20]
                # Re-resolve on every attribute: a PM suspend/resume
                # re-registers the services at fresh handles, and the
                # highest (latest) registration must win.
                self._resolve_handles()
            return
        if op == EVT_CONNECTED:
            self._connected_evt.set()
            return
        if op == EVT_PAIRED:
            self._paired_evt.set()
            return
        if op == EVT_DISCONNECTED:
            with self._state_lock:
                client, self._client = self._client, None
            if client:
                self.log("guest dropped the connection")
                _hard_close(client)
            return
        if op == EVT_NOTIFY and self._handles:
            hdl, _evt_type = struct.unpack("<HB", payload[:3])
            data = payload[3:]
            h = self._handles
            if hdl == h.get("tx"):
                self._client_channel_send(CH_LUA, data)
            elif hdl == h.get("audio_tx"):
                self._client_channel_send(CH_AUDIO, data)
            elif hdl == h.get("video"):
                self._client_channel_send(CH_VIDEO, data)
            return
        # EVT_WRITE_STATUS / EVT_READ_RSP / EVT_ADV_*: nothing to do

    def _doorbell_loop(self):
        buf = b""
        while not self._stop:
            data = self._doorbell.recv(65536)
            if not data:
                self.log("doorbell closed (QEMU exited)")
                break
            buf += data
            while len(buf) >= 4:
                op, _flags, ln = struct.unpack("<BBH", buf[:4])
                if len(buf) < 4 + ln:
                    break
                self._handle_evt(op, buf[4:4 + ln])
                buf = buf[4 + ln:]
        # QEMU is gone: drop the client so it sees EOF, then stop
        with self._state_lock:
            client, self._client = self._client, None
        if client:
            _hard_close(client)
        self._stop = True

    # ---------------------------------------------------------------- #
    # Client side                                                       #
    # ---------------------------------------------------------------- #

    def _attach_client(self, sock):
        """Connect the guest for a fresh client; returns True when the
        REPL is ready for traffic."""
        if not self._db_ready.wait(timeout=60):
            self.log("GATT database never arrived — is the firmware up?")
            return False
        self._connected_evt.clear()
        self._paired_evt.clear()
        self._doorbell_send(OP_CONNECT,
                            bytes([0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01]))
        if not (self._connected_evt.wait(timeout=10) and
                self._paired_evt.wait(timeout=10)):
            self.log("guest did not complete connect/pair")
            return False
        h = self._handles
        for name in ("tx_ccc", "video_ccc", "audio_tx_ccc"):
            if name in h:
                self._gatt_write(h[name], b"\x01\x00")
        return True

    def _client_loop(self, sock):
        buf = b""
        while not self._stop:
            try:
                data = sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while len(buf) >= 3:
                channel, ln = struct.unpack("<BH", buf[:3])
                if ln > MTU:
                    self.log(f"client payload {ln} exceeds MTU {MTU} — "
                             "dropping client")
                    return
                if len(buf) < 3 + ln:
                    break
                payload = buf[3:3 + ln]
                buf = buf[3 + ln:]
                h = self._handles
                if h is None:
                    continue  # rebooting under the client's feet
                if channel == CH_LUA:
                    self._gatt_write(h["rx"], payload)
                elif channel == CH_AUDIO and "audio_rx" in h:
                    self._gatt_write(h["audio_rx"], payload)
                else:
                    self.log(f"frame on invalid channel {channel} ignored")

    def _run_client(self, sock):
        try:
            if self._attach_client(sock):
                self.log("client connected")
                self._client_loop(sock)
        finally:
            with self._state_lock:
                still_ours = self._client is sock
                if still_ours:
                    self._client = None
            sock.close()
            if still_ours and not self._stop:
                # Tell the guest its central went away (0x13: remote
                # user terminated connection).
                try:
                    self._doorbell_send(OP_DISCONNECT,
                                        struct.pack("<H", 0x13))
                except OSError:
                    pass
            self.log("client disconnected")

    def _serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Loopback only: the REPL is arbitrary code execution.
        srv.bind(("127.0.0.1", self.listen_port))
        srv.listen(2)
        srv.settimeout(0.5)
        while not self._stop:
            try:
                sock, _addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._state_lock:
                busy = self._client is not None
                if not busy:
                    self._client = sock
            if busy:
                sock.close()  # single-client policy
                self.log("second client refused")
                continue
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self._run_client, args=(sock,),
                             name="repl-bridge-client", daemon=True).start()
        srv.close()

    # ---------------------------------------------------------------- #

    def run(self):
        try:
            self._doorbell = self._dial_doorbell()
        except OSError as e:
            self.log(f"cannot reach the doorbell chardev: {e}")
            return
        t = threading.Thread(target=self._doorbell_loop,
                             name="repl-bridge-doorbell", daemon=True)
        t.start()
        try:
            self._serve()
        finally:
            self._stop = True

    def stop(self):
        self._stop = True
        try:
            if self._doorbell:
                self._doorbell.close()
        except OSError:
            pass
