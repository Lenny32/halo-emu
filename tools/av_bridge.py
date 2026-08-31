#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "aiohttp>=3.9",
#     "cryptography>=42",
#     "qrcode>=7.4",
# ]
# ///
"""av_bridge.py — web A/V bridge for the halo emulator (ticket 0040).

Serves one HTTPS listener (default port 9564) with:

    /            desktop UI: live 256x256 panel mirror, control verbs,
                 phone onboarding QR, speaker playback
    /phone       phone capture page: getUserMedia mic + camera, streamed
                 to the emulator with no app install (self-signed cert,
                 tap through the warning once)
    /ws/control  JSON: ctl-verb pass-through + 2 Hz status snapshots
    /ws/display  binary down: <II> w,h LE + RGB888 (changed frames only)
    /ws/speaker  text {"rate":N}, then binary s16le mono chunks
    /ws/phone    text hello {"mic":{"rate":N},"cam":{...}}, then tagged
                 binary up: 0x01 + s16le PCM, 0x02 + <HH> w,h + RGB888

Emulator plumbing (no QEMU or launcher changes):

    camera   phone frame -> HALOCAM1 container (RGB888) -> atomic
             os.replace -> qom-set /machine camera-file.  The LPCAM
             model re-reads the file on every set, so a 1-frame
             container is a live "current frame" latch that the guest's
             Lua capture() samples.
    mic      v1 is chunked: ~600 ms WAVs on alternating paths, swapped
             via qom-set mic-wav-in once the guest has consumed the
             previous chunk (mic-samples delta) or a wall-clock timeout
             passes.  Underrun sets "" (silence) so a stalled phone
             never loops stale audio.  True streaming = follow-up
             chardev ticket.
    speaker  tails the --wav-out file (valid WAV at all times, s16le
             mono 32 kHz) from offset 44.
    display  QMP screendump (P6 PPM) polled at --display-fps.
    control  the bridge holds the single ctl-socket client (unit
             conversions live there) and its own second QMP monitor for
             the hot paths, passed via the launcher's trailing args.

Usage:
    tools/av_bridge.py [-f FW | --fetch VER]      spawn the emulator
    tools/av_bridge.py --attach <qmp.sock> [--wav-out-path FILE]
    tools/av_bridge.py --smoke [-f FW]            no-phone self-test

`--public-ip` overrides the advertised address (WSL2: use the Windows
host's LAN IP and forward the port, or use networkingMode=mirrored).
"""

import argparse
import array
import asyncio
import datetime
import ipaddress
import json
import os
import socket
import ssl
import struct
import sys
import tempfile
import time
import wave
import zlib

try:
    from aiohttp import ClientSession, WSMsgType, web
except ImportError:
    sys.exit(
        "av_bridge: missing dependencies (aiohttp, cryptography, qrcode).\n"
        "This is a uv script — run it as `tools/av_bridge.py` or "
        "`uv run tools/av_bridge.py`\n"
        "(install uv: curl -LsSf https://astral.sh/uv/install.sh | sh), "
        "or install the three\npackages into your own environment and use "
        "python3 directly.")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
WEBUI = os.path.join(REPO, "tools", "webui")
AVDIR = os.path.join(REPO, ".avbridge")

sys.path.insert(0, os.path.join(REPO, "tools"))
from fetch_firmware import default_firmware  # noqa: E402

MIC_RATE = 16000            # the firmware's frame.microphone rate
MIC_CHUNK_MS = 600          # v1 swap granularity
MIC_BUF_MS = 3000           # ring cap; drop oldest beyond this
WS_MAX_MSG = 8 * 1024 * 1024


# --- blocking wire clients (the launcher / smoke-test pattern) ----------

class Qmp:
    """Minimal QMP client over a unix socket (halo-emu's own)."""

    def __init__(self, path, timeout=10.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""
        self._read_msg()  # greeting
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
            # asynchronous event — skip


class Ctl:
    """Control-socket client: one text verb per line, `ok`/`err` reply
    returned verbatim (the UI pass-through shows errors to the user)."""

    def __init__(self, port, timeout=90.0):
        self.sock = socket.create_connection(("127.0.0.1", port),
                                             timeout=timeout)
        self.f = self.sock.makefile("rw", encoding="utf-8", newline="\n")

    def cmd(self, line):
        self.f.write(line + "\n")
        self.f.flush()
        reply = self.f.readline().strip()
        if not reply:
            raise ConnectionError("ctl socket closed")
        return reply

    def close(self):
        self.sock.close()


class AsyncWire:
    """Serialised, lazily-dialled, re-dialled-on-error wrapper that runs
    a blocking client in the default executor (QEMU may still be
    starting when the bridge comes up)."""

    def __init__(self, dial, deadline=60.0):
        self._dial = dial
        self._deadline = deadline
        self._client = None
        self._lock = asyncio.Lock()

    def _connect(self):
        deadline = time.monotonic() + self._deadline
        while True:
            try:
                return self._dial()
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)

    def _call(self, fn, *args, **kwargs):
        if self._client is None:
            self._client = self._connect()
        try:
            return fn(self._client, *args, **kwargs)
        except (OSError, ConnectionError):
            self._client = None  # dropped (emulator restart): once more
            self._client = self._connect()
            return fn(self._client, *args, **kwargs)

    async def call(self, fn, *args, **kwargs):
        loop = asyncio.get_running_loop()
        async with self._lock:
            return await loop.run_in_executor(
                None, lambda: self._call(fn, *args, **kwargs))


class AsyncQmp(AsyncWire):
    def __init__(self, path, deadline=60.0):
        super().__init__(lambda: Qmp(path), deadline)

    async def command(self, name, **arguments):
        return await self.call(lambda q: q.command(name, **arguments))

    async def qom_get(self, prop):
        return await self.command("qom-get", path="/machine", property=prop)

    async def qom_set(self, prop, value):
        return await self.command("qom-set", path="/machine",
                                  property=prop, value=value)


class AsyncCtl(AsyncWire):
    def __init__(self, port, deadline=60.0):
        super().__init__(lambda: Ctl(port), deadline)

    async def cmd(self, line):
        return await self.call(lambda c: c.cmd(line))


def read_ppm(data):
    """Parse a binary P6 PPM into (width, height, pixel bytes)."""
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 PPM: {data[:16]!r}")
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
        raise ValueError(f"unexpected PPM maxval {maxval}")
    return w, h, data[pos:pos + w * h * 3]


# --- TLS -----------------------------------------------------------------

def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))  # no packet is sent; routing only
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def ensure_cert(addresses):
    """Self-signed ECDSA P-256 cert in .avbridge/, regenerated when an
    advertised address is missing from the SAN or it has expired."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    os.makedirs(AVDIR, exist_ok=True)
    cert_path = os.path.join(AVDIR, "cert.pem")
    key_path = os.path.join(AVDIR, "key.pem")

    sans = []
    for a in dict.fromkeys(["127.0.0.1", "localhost",
                            socket.gethostname()] + addresses):
        try:
            sans.append(x509.IPAddress(ipaddress.ip_address(a)))
        except ValueError:
            sans.append(x509.DNSName(a))

    if os.path.exists(cert_path) and os.path.exists(key_path):
        try:
            cert = x509.load_pem_x509_certificate(
                open(cert_path, "rb").read())
            have = cert.extensions.get_extension_for_class(
                x509.SubjectAlternativeName).value
            now = datetime.datetime.now(datetime.timezone.utc)
            if (all(s in have for s in sans)
                    and now < cert.not_valid_after_utc):
                return cert_path, key_path
        except Exception:
            pass  # unreadable/foreign cert: regenerate

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                         "halo-av-bridge")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(name).issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.SubjectAlternativeName(sans),
                           critical=False)
            .sign(key, hashes.SHA256()))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    print(f"av_bridge: generated self-signed cert in {AVDIR}",
          file=sys.stderr)
    return cert_path, key_path


# --- audio ---------------------------------------------------------------

class Resampler:
    """Stateful linear resampler for s16le mono, phase carried across
    chunks (the phone's AudioContext may refuse the 16 kHz hint)."""

    def __init__(self, src, dst=MIC_RATE):
        self.src, self.dst = src, dst
        self.pos = 0.0
        self.last = 0

    def feed(self, data):
        if self.src == self.dst:
            return data
        chunk = array.array("h")
        chunk.frombytes(data[:len(data) & ~1])
        if not chunk:
            return b""
        arr = array.array("h", [self.last])
        arr.extend(chunk)
        step = self.src / self.dst
        out = array.array("h")
        pos, limit = self.pos, len(arr) - 1
        while pos < limit:
            j = int(pos)
            frac = pos - j
            out.append(int(arr[j] + (arr[j + 1] - arr[j]) * frac))
            pos += step
        self.last = arr[-1]
        self.pos = pos - limit
        return out.tobytes()


def write_wav(path, pcm, rate=MIC_RATE):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def parse_kv(reply):
    """`ok raw=3980 mv=4198 pct=95` -> {'raw': '3980', ...}"""
    out = {}
    for tok in reply.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


# --- the bridge ----------------------------------------------------------

class Bridge:
    def __init__(self, args, qmp, ctl, scratch, wav_out_path):
        self.args = args
        self.qmp = qmp
        self.ctl = ctl
        self.scratch = scratch
        self.wav_out_path = wav_out_path
        self.phone_url = None  # set once the listener is up

        self.control_clients = set()
        self.display_clients = set()
        self.speaker_clients = set()
        self.phone_ws = None
        self.phone_info = None

        self.camera_fps = args.camera_fps
        self.cam_latest = None
        self.cam_pushed = 0
        self._cam_last = 0.0
        self._cam_path = os.path.join(scratch, "phone.cam")

        self.mic_buf = bytearray()
        self.mic_resampler = None
        self.mic_last_data = 0.0
        self._mic_idx = 0
        self._mic_chunk_len = 0      # samples in the chunk now playing
        self._mic_set_time = 0.0
        self._mic_samples_at_set = 0
        self._mic_active = False     # we own mic-wav-in right now

        self.speaker_rate = None
        self.display_dirty = True  # rebroadcast to a fresh client
        self.emu_proc = None
        self.emu_exited = asyncio.Event()

    # --- fan-out ---------------------------------------------------

    async def _broadcast(self, clients, payload, binary):
        dead = []
        for ws in clients:
            try:
                if binary:
                    await ws.send_bytes(payload)
                else:
                    await ws.send_json(payload)
            except (ConnectionError, RuntimeError):
                dead.append(ws)
        for ws in dead:
            clients.discard(ws)

    async def event(self, name):
        await self._broadcast(self.control_clients, {"event": name},
                              binary=False)

    # --- background loops ------------------------------------------

    async def display_loop(self):
        ppm = os.path.join(self.scratch, "frame.ppm")
        period = 1.0 / self.args.display_fps
        last_crc = None
        while True:
            t0 = time.monotonic()
            try:
                await self.qmp.command("screendump", filename=ppm)
                w, h, rgb = read_ppm(open(ppm, "rb").read())
                crc = zlib.crc32(rgb)
                if ((crc != last_crc or self.display_dirty)
                        and self.display_clients):
                    last_crc = crc
                    self.display_dirty = False
                    await self._broadcast(
                        self.display_clients,
                        struct.pack("<II", w, h) + rgb, binary=True)
            except Exception:
                pass  # emulator still starting / restarting
            await asyncio.sleep(max(0.0, period - (time.monotonic() - t0)))

    async def status_loop(self):
        while True:
            status = {"bridge": {
                "phone": self.phone_ws is not None,
                "camera_fps": self.camera_fps,
                "camera_pushed": self.cam_pushed,
                "mic_buffer_ms": len(self.mic_buf) // 2 * 1000 // MIC_RATE,
                "speaker_rate": self.speaker_rate,
                "speaker": self.wav_out_path is not None,
                "phone_url": self.phone_url,
            }}
            for key, verb in (("battery", "battery?"), ("charger",
                              "charger?"), ("mic", "mic?"),
                              ("camera", "camera?"), ("speaker",
                              "speaker?"), ("led", "led?")):
                try:
                    status[key] = parse_kv(await self.ctl.cmd(verb))
                except Exception:
                    status[key] = None
            if self.control_clients:
                await self._broadcast(self.control_clients,
                                      {"status": status}, binary=False)
            await asyncio.sleep(0.5)

    async def camera_loop(self):
        tmp = self._cam_path + ".tmp"
        while True:
            await asyncio.sleep(0.02)
            if self.cam_latest is None:
                continue
            now = time.monotonic()
            if now - self._cam_last < 1.0 / max(self.camera_fps, 0.1):
                continue
            w, h, rgb = self.cam_latest
            self.cam_latest = None
            self._cam_last = now
            try:
                with open(tmp, "wb") as f:
                    f.write(b"HALOCAM1"
                            + struct.pack("<IIII", w, h, 1, 0) + rgb)
                os.replace(tmp, self._cam_path)
                await self.qmp.qom_set("camera-file", self._cam_path)
                self.cam_pushed += 1
            except Exception as e:
                print(f"av_bridge: camera push failed: {e}",
                      file=sys.stderr)

    async def mic_loop(self):
        chunk_bytes = MIC_RATE * MIC_CHUNK_MS // 1000 * 2
        chunk_samples = chunk_bytes // 2
        chunk_s = MIC_CHUNK_MS / 1000.0
        while True:
            await asyncio.sleep(0.1)
            now = time.monotonic()
            if len(self.mic_buf) >= chunk_bytes:
                # Swap once the guest has mostly consumed the current
                # chunk on its own (virtual) clock, or after a
                # wall-clock fallback (covers mic-off guests).
                ready = not self._mic_active
                if not ready:
                    try:
                        consumed = (await self.qmp.qom_get("mic-samples")
                                    - self._mic_samples_at_set)
                    except Exception:
                        consumed = 0
                    ready = (consumed >= self._mic_chunk_len * 9 // 10
                             or now - self._mic_set_time >= 2 * chunk_s)
                if ready:
                    pcm = bytes(self.mic_buf[:chunk_bytes])
                    del self.mic_buf[:chunk_bytes]
                    # Alternate paths: never rewrite the file the model
                    # is currently looping.
                    self._mic_idx ^= 1
                    path = os.path.join(self.scratch,
                                        f"mic{self._mic_idx}.wav")
                    try:
                        write_wav(path, pcm)
                        await self.qmp.qom_set("mic-wav-in", path)
                        self._mic_samples_at_set = \
                            await self.qmp.qom_get("mic-samples")
                        self._mic_chunk_len = chunk_samples
                        self._mic_set_time = now
                        self._mic_active = True
                    except Exception as e:
                        print(f"av_bridge: mic swap failed: {e}",
                              file=sys.stderr)
            elif (self._mic_active
                  and now - self.mic_last_data > 1.5 * chunk_s):
                # Underrun: silence beats looping a stale chunk.
                try:
                    await self.qmp.qom_set("mic-wav-in", "")
                except Exception:
                    pass
                self._mic_active = False

    async def speaker_loop(self):
        path = self.wav_out_path
        if path is None:
            return
        while not (os.path.exists(path) and os.path.getsize(path) >= 44):
            await asyncio.sleep(0.2)
        with open(path, "rb") as f:
            hdr = f.read(44)
            self.speaker_rate = int.from_bytes(hdr[24:28], "little")
            offset = 44
            while True:
                size = os.path.getsize(path)
                if size > offset:
                    f.seek(offset)
                    data = f.read(size - offset)
                    offset += len(data)
                    if self.speaker_clients:
                        await self._broadcast(self.speaker_clients, data,
                                              binary=True)
                await asyncio.sleep(0.05)

    # --- websocket handlers -----------------------------------------

    async def ws_control(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.control_clients.add(ws)
        try:
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    continue
                try:
                    obj = json.loads(msg.data)
                except ValueError:
                    continue
                if "ctl" in obj:
                    try:
                        reply = await self.ctl.cmd(str(obj["ctl"]))
                    except Exception as e:
                        reply = f"err bridge: {e}"
                    await ws.send_json({"reply": reply,
                                        "id": obj.get("id")})
                elif obj.get("cmd") == "shutdown":
                    await ws.send_json({"reply": "ok shutting down",
                                        "id": obj.get("id")})
                    try:
                        # The socket dies with QEMU: a dropped
                        # connection here is success, not an error.
                        await self.qmp.command("quit")
                    except (OSError, ConnectionError, RuntimeError):
                        pass
                elif obj.get("cmd") == "camera-fps":
                    try:
                        self.camera_fps = max(0.2, min(15.0,
                                              float(obj["fps"])))
                        await ws.send_json({"reply": "ok",
                                            "id": obj.get("id")})
                    except (KeyError, ValueError):
                        await ws.send_json({"reply": "err bad fps",
                                            "id": obj.get("id")})
        finally:
            self.control_clients.discard(ws)
        return ws

    async def ws_display(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.display_clients.add(ws)
        self.display_dirty = True
        try:
            async for _ in ws:
                pass
        finally:
            self.display_clients.discard(ws)
        return ws

    async def ws_speaker(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        await ws.send_json({"rate": self.speaker_rate or 32000,
                            "enabled": self.wav_out_path is not None})
        self.speaker_clients.add(ws)
        try:
            async for _ in ws:
                pass
        finally:
            self.speaker_clients.discard(ws)
        return ws

    async def ws_phone(self, request):
        ws = web.WebSocketResponse(max_msg_size=WS_MAX_MSG)
        await ws.prepare(request)
        if self.phone_ws is not None:
            await self.phone_ws.close()  # newest phone wins
        self.phone_ws = ws
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    try:
                        hello = json.loads(msg.data).get("hello", {})
                    except ValueError:
                        continue
                    rate = int(hello.get("mic", {}).get("rate", MIC_RATE))
                    self.mic_resampler = Resampler(rate)
                    self.phone_info = hello
                    await self.event("phone-connected")
                elif msg.type == WSMsgType.BINARY and msg.data:
                    tag, body = msg.data[0], msg.data[1:]
                    if tag == 0x01 and self.mic_resampler is not None:
                        pcm = self.mic_resampler.feed(body)
                        self.mic_buf.extend(pcm)
                        cap = MIC_RATE * MIC_BUF_MS // 1000 * 2
                        if len(self.mic_buf) > cap:
                            del self.mic_buf[:len(self.mic_buf) - cap]
                        self.mic_last_data = time.monotonic()
                    elif tag == 0x02 and len(body) >= 4:
                        w, h = struct.unpack("<HH", body[:4])
                        rgb = body[4:]
                        if w and h and len(rgb) == w * h * 3:
                            self.cam_latest = (w, h, bytes(rgb))
        finally:
            if self.phone_ws is ws:
                self.phone_ws = None
                self.phone_info = None
                await self.event("phone-lost")
        return ws

    # --- http --------------------------------------------------------

    def _page(self, name):
        async def handler(request):
            return web.FileResponse(os.path.join(WEBUI, name))
        return handler

    async def qr_svg(self, request):
        import qrcode
        import qrcode.image.svg
        from io import BytesIO
        img = qrcode.make(self.phone_url or "",
                          image_factory=qrcode.image.svg.SvgPathImage)
        buf = BytesIO()
        img.save(buf)
        return web.Response(body=buf.getvalue(),
                            content_type="image/svg+xml")

    def make_app(self):
        app = web.Application()
        app.router.add_get("/", self._page("index.html"))
        app.router.add_get("/phone", self._page("phone.html"))
        app.router.add_get("/qr.svg", self.qr_svg)
        app.router.add_get("/ws/control", self.ws_control)
        app.router.add_get("/ws/display", self.ws_display)
        app.router.add_get("/ws/speaker", self.ws_speaker)
        app.router.add_get("/ws/phone", self.ws_phone)
        app.router.add_static("/webui", WEBUI)
        return app


# --- emulator spawn -------------------------------------------------------

async def spawn_emulator(args, extra, scratch, qmp_sock, wav_out_path):
    cmd = [HALO_EMU]
    if args.fetch:
        cmd += ["--fetch", args.fetch]
    else:
        cmd += ["-f", args.firmware]
    cmd += ["--headless", "--wav-out", wav_out_path,
            "--ctl-port", str(args.ctl_port)]
    if args.smoke:
        # Never touch the user's persistent ./mram.img from the self-test.
        cmd += ["--flash", os.path.join(scratch, "mram.img")]
    cmd += extra
    cmd += ["--", "-qmp", f"unix:{qmp_sock},server=on,wait=off"]
    print("av_bridge: spawning:", " ".join(cmd), file=sys.stderr)
    return await asyncio.create_subprocess_exec(
        *cmd, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT)


async def pump_emulator(bridge):
    proc = bridge.emu_proc
    assert proc.stdout is not None
    while True:
        line = await proc.stdout.readline()
        if not line:
            break
        sys.stderr.write("emu | " + line.decode(errors="replace"))
    await proc.wait()
    print(f"av_bridge: emulator exited ({proc.returncode})",
          file=sys.stderr)
    await bridge.event("emu-exit")
    bridge.emu_exited.set()


# --- smoke ----------------------------------------------------------------

async def poll(what, fn, timeout=10.0, gap=0.25):
    deadline = time.monotonic() + timeout
    while True:
        try:
            if await fn():
                print(f"smoke: PASS — {what}")
                return
        except Exception:
            pass
        if time.monotonic() >= deadline:
            raise AssertionError(f"smoke: FAIL — {what}")
        await asyncio.sleep(gap)


async def run_smoke(bridge, port):
    import math
    sslctx = ssl.create_default_context()
    sslctx.check_hostname = False
    sslctx.verify_mode = ssl.CERT_NONE
    base = f"wss://127.0.0.1:{port}"

    async with ClientSession() as sess:
        disp = await sess.ws_connect(base + "/ws/display", ssl=sslctx)
        phone = await sess.ws_connect(base + "/ws/phone", ssl=sslctx,
                                      max_msg_size=WS_MAX_MSG)
        await phone.send_json({"hello": {"mic": {"rate": MIC_RATE},
                                         "cam": {"w": 320, "h": 240,
                                                 "fps": 5}}})

        # one gradient camera frame
        w, h = 320, 240
        row = bytes(b for x in range(w)
                    for b in (x * 255 // w, 128, 255 - x * 255 // w))
        await phone.send_bytes(b"\x02" + struct.pack("<HH", w, h)
                               + row * h)
        # camera-width/height report the guest-programmed geometry, so
        # "did the frame land" is camera-source flipping to "file".
        await poll("camera frame latched (source=file)",
                   lambda: _cam_latched(bridge))

        # 1.2 s of 1 kHz tone (not DC — the Lua mic path DC-blocks)
        tone = array.array("h", (
            int(12000 * math.sin(2 * math.pi * 1000 * i / MIC_RATE))
            for i in range(MIC_RATE * 12 // 10))).tobytes()
        for i in range(0, len(tone), 3200):
            await phone.send_bytes(b"\x01" + tone[i:i + 3200])
            await asyncio.sleep(0.1)
        await poll("mic source=wav after chunk swap",
                   lambda: _mic_source(bridge, "wav"))
        await poll("mic falls back to silence on underrun",
                   lambda: _mic_source(bridge, "silence"), timeout=15.0)

        msg = await asyncio.wait_for(disp.receive(), 15.0)
        assert msg.type == WSMsgType.BINARY, f"display sent {msg.type}"
        dw, dh = struct.unpack_from("<II", msg.data)
        assert (dw, dh) == (256, 256), f"panel {dw}x{dh}"
        print("smoke: PASS — display frame 256x256 received")

        reply = await bridge.ctl.cmd("battery set 82%")
        assert reply.startswith("ok"), reply
        kv = parse_kv(await bridge.ctl.cmd("battery?"))
        assert abs(int(kv["pct"]) - 82) <= 2, kv
        print("smoke: PASS — ctl round-trip (battery 82%)")

        await phone.close()
        await disp.close()
    print("av_bridge --smoke: all checks passed")


async def _cam_latched(bridge):
    return (await bridge.qmp.qom_get("camera-source") == "file"
            and bridge.cam_pushed >= 1)


async def _mic_source(bridge, want):
    return parse_kv(await bridge.ctl.cmd("mic?")).get("source") == want


# --- main -----------------------------------------------------------------

def parse_args(argv):
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    p = argparse.ArgumentParser(
        description="Web A/V bridge for halo-emu: phone mic/camera in, "
                    "browser UI. Args after -- go to halo-emu.")
    p.add_argument("-f", "--firmware", metavar="BIN",
                   help="firmware image (default: the cached calibration "
                        "release, like the smoke tests)")
    p.add_argument("--fetch", metavar="VERSION",
                   help="let halo-emu download this release instead of -f")
    p.add_argument("--attach", metavar="QMP_SOCK",
                   help="attach to a running emulator via this QMP unix "
                        "socket (started with -- -qmp unix:...,server=on,"
                        "wait=off) instead of spawning one")
    p.add_argument("--ctl-port", type=int, default=9562,
                   help="emulator control socket port (default 9562)")
    p.add_argument("--wav-out-path", metavar="FILE",
                   help="attach mode: the running emulator's --wav-out "
                        "file, enables speaker streaming")
    p.add_argument("--port", type=int, default=9564,
                   help="HTTPS listen port (default 9564)")
    p.add_argument("--public-ip", metavar="ADDR",
                   help="address to advertise in the phone URL/QR and "
                        "cert (WSL2: the Windows host's LAN IP)")
    p.add_argument("--camera-fps", type=float, default=5.0,
                   help="max phone-camera frames pushed per second "
                        "(default 5)")
    p.add_argument("--display-fps", type=float, default=10.0,
                   help="panel mirror poll rate (default 10)")
    p.add_argument("--smoke", action="store_true",
                   help="no-phone self-test: spawn, feed synthetic A/V "
                        "over wss, assert, exit")
    return p.parse_args(argv), extra


async def async_main():
    args, extra = parse_args(sys.argv[1:])
    if args.attach and (args.fetch or args.firmware):
        print("av_bridge: --attach excludes -f/--fetch", file=sys.stderr)
        return 2
    if not args.attach and not args.fetch:
        args.firmware = args.firmware or default_firmware()
        if not os.path.exists(args.firmware):
            print(f"av_bridge: no firmware at {args.firmware} — pass -f, "
                  "--fetch <version>, or run tools/fetch_firmware.py",
                  file=sys.stderr)
            return 2

    scratch = tempfile.mkdtemp(prefix="halo-avbridge-")
    host_ip = args.public_ip or lan_ip()
    cert, key = ensure_cert([host_ip] + ([lan_ip()]
                            if args.public_ip else []))
    sslctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    sslctx.load_cert_chain(cert, key)

    if args.attach:
        qmp_sock = args.attach
        wav_out_path = args.wav_out_path
    else:
        qmp_sock = os.path.join(scratch, "qmp.sock")
        wav_out_path = os.path.join(scratch, "speaker.wav")

    qmp = AsyncQmp(qmp_sock)
    ctl = AsyncCtl(args.ctl_port)
    bridge = Bridge(args, qmp, ctl, scratch, wav_out_path)

    if not args.attach:
        bridge.emu_proc = await spawn_emulator(args, extra, scratch,
                                               qmp_sock, wav_out_path)

    runner = web.AppRunner(bridge.make_app())
    await runner.setup()
    site = web.TCPSite(runner, "0.0.0.0", args.port, ssl_context=sslctx)
    await site.start()
    bridge.phone_url = f"https://{host_ip}:{args.port}/phone"

    tasks = [asyncio.create_task(t()) for t in (
        bridge.display_loop, bridge.status_loop, bridge.camera_loop,
        bridge.mic_loop, bridge.speaker_loop)]
    if bridge.emu_proc:
        tasks.append(asyncio.create_task(pump_emulator(bridge)))

    print(f"\nav_bridge: desktop UI   https://127.0.0.1:{args.port}/",
          file=sys.stderr)
    print(f"av_bridge: phone page   {bridge.phone_url}", file=sys.stderr)
    if os.path.exists("/proc/version") and \
            "microsoft" in open("/proc/version").read().lower():
        print("av_bridge: WSL2 detected — your phone cannot reach this "
              "address directly.\n"
              "  Either set networkingMode=mirrored in .wslconfig, or on "
              "Windows run (admin):\n"
              f"    netsh interface portproxy add v4tov4 "
              f"listenport={args.port} connectaddress={lan_ip()} "
              f"connectport={args.port}\n"
              "  then relaunch with --public-ip <Windows LAN IP>.",
              file=sys.stderr)
    try:
        import qrcode
        q = qrcode.QRCode(border=1)
        q.add_data(bridge.phone_url)
        q.print_ascii(invert=True)
    except Exception:
        pass

    rc = 0
    try:
        if args.smoke:
            rc = 1
            await asyncio.wait_for(run_smoke(bridge, args.port), 180.0)
            rc = 0
        elif bridge.emu_proc:
            await bridge.emu_exited.wait()
        else:
            await asyncio.Event().wait()  # attach mode: run until ^C
    finally:
        for t in tasks:
            t.cancel()
        if bridge.emu_proc and bridge.emu_proc.returncode is None:
            bridge.emu_proc.terminate()
            try:
                await asyncio.wait_for(bridge.emu_proc.wait(), 10)
            except asyncio.TimeoutError:
                bridge.emu_proc.kill()
        await runner.cleanup()
    return rc


def main():
    try:
        sys.exit(asyncio.run(async_main()))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
