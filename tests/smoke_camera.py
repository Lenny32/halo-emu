#!/usr/bin/env python3
"""smoke_camera.py — end-to-end gate of the camera path (ticket 0033)
against a real firmware image.

Boots `halo-emu -f <firmware> --camera test-pattern` headless and drives
the Lua camera API over the REPL (tcp://127.0.0.1:9563) while injecting
and inspecting sources over the control socket (tcp://127.0.0.1:9562):

    1. asleep at boot     the camera service starts in power save, so
                          nothing has touched the sensor: no captures, no
                          I2C triggers, no format programmed
    2. capture            frame.camera.power_save(false) + capture() runs
                          three frames (LUA_CAMERA_SKIP_FRAMES, the last
                          one kept) and triggers the PAG7982 over I2C —
                          proof the sensor model answered its part-ID read
    3. raw frame          read_raw() returns the injected image *exactly*,
                          byte for byte against the Bayer BGGR8 mosaic
                          the LPCAM model is supposed to have DMA'd
    4. JPEG round trip    the firmware's own libmpix debayer + JPEG encode
                          is decoded back here and every colour bar comes
                          out with the right channels high.  Absolute
                          values drift because libmpix applies automatic
                          black-level and white-balance correction, so the
                          assertion is the high/low signature, which is
                          unique per bar
    5. runtime swap       `camera <file>` replaces the source without a
                          reboot (PNG and PPM, decoded here — this QEMU
                          has no libpng)
    6. gradient fallback  `camera none` falls back to the LPCAM model's
                          built-in gradient, so frame.camera keeps working
                          with no --camera given at all
    7. mjpeg              the two JPEGs the firmware produced are
                          concatenated into an MJPEG and fed back in, so
                          the multi-frame source and its per-frame advance
                          are covered without needing a JPEG encoder here
    8. bad source         a broken file is refused without disturbing the
                          running source, and the guest survives

Bulk pixel data comes back over the REPL's 0x01 data channel via
frame.bluetooth.send rather than hex through print(): a whole 24 KB JPEG
crosses in well under a second.

Usage: tests/smoke_camera.py [-f firmware.bin] [--repl-port 9563]
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
from smoke_controls import Ctl  # noqa: E402
import camera_source as cs  # noqa: E402

# The sensor's one format (pag7982.c fmts[]); the firmware asks for it
# and Lua refuses any other resolution.
WIDTH, HEIGHT = cs.SENSOR_WIDTH, cs.SENSOR_HEIGHT
ROW = WIDTH          # one Bayer row = one byte per pixel
SKIP_FRAMES = 3      # LUA_CAMERA_SKIP_FRAMES in lua_camera.c

# Flat quadrants for the runtime-swap checks: large uniform areas survive
# debayer and JPEG, so their colours stay readable.
QUADRANTS = [(200, 40, 40), (40, 200, 40), (40, 40, 200), (220, 220, 40)]


def check(name, ok, detail=""):
    print(f"smoke_camera: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def fields(reply):
    """`ok x=1 y=2 ...` -> {'x': '1', ...} (ignores bare tokens)."""
    return dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)


def quadrant_image():
    """640x480 RGB888 split into four flat colour quadrants."""
    rgb = bytearray(WIDTH * HEIGHT * 3)
    for y in range(HEIGHT):
        top = 0 if y < HEIGHT // 2 else 2
        for x in range(WIDTH):
            c = QUADRANTS[top + (1 if x >= WIDTH // 2 else 0)]
            o = (y * WIDTH + x) * 3
            rgb[o:o + 3] = bytes(c)
    return bytes(rgb)


def capture(r, quality="VERY_HIGH", tries=25):
    """Trigger a capture and wait for image_ready()."""
    r.lua(f"frame.camera.capture{{quality='{quality}'}} print('go')")
    for _ in range(tries):
        time.sleep(0.4)
        if r.lua("print(tostring(frame.camera.image_ready()))") == "true":
            return True
    return False


def fetch(r, expr, limit, timeout=25.0):
    """Pull up to `limit` bytes out of the guest over the data channel.

    `expr` is a Lua expression returning a string or nil (one of the
    frame.camera readers); the loop inside the guest keeps sending until
    it runs dry or hits the limit.
    """
    out = bytearray()
    r.drain(0.3)
    r.send(0, ("local n = 0 "
               "while true do local s = %s if s == nil then break end "
               "frame.bluetooth.send(s) n = n + #s "
               "if n >= %d then break end end "
               "print(string.format('sent=%%d', n))"
               % (expr, limit)).encode())
    for ch, pay in r.frames(timeout):
        if ch == 0 and pay[:1] == b"\x01":
            out += pay[1:]
        elif ch == 0 and pay.startswith(b"sent="):
            break
    return bytes(out[:limit])


def raw_cmd(ctl, line):
    """Like Ctl.cmd but returns `err` replies instead of raising."""
    ctl.f.write(line + "\n")
    ctl.f.flush()
    return ctl.f.readline().strip()


def signature(rgb, width, x, y):
    """(high, high, low)-style channel signature at one pixel."""
    o = (y * width + x) * 3
    return tuple(v > 128 for v in rgb[o:o + 3])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"))
    p.add_argument("--repl-port", type=int, default=9563)
    p.add_argument("--ctl-port", type=int, default=9562)
    p.add_argument("--boot-wait", type=float, default=20.0)
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"smoke_camera: firmware not found: {args.firmware}")

    workdir = tempfile.mkdtemp(prefix="halo-camera-smoke-")
    flash = os.path.join(workdir, "mram.img")
    quad = quadrant_image()
    png_path = os.path.join(workdir, "quad.png")
    open(png_path, "wb").write(cs.encode_png(quad, WIDTH, HEIGHT))
    ppm_path = os.path.join(workdir, "quad.ppm")
    with open(ppm_path, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
        fh.write(quad)
    bad_path = os.path.join(workdir, "broken.png")
    open(bad_path, "wb").write(b"\x89PNG\r\n\x1a\nnot really a png")

    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware, "--flash", flash, "--headless",
         "--camera", "test-pattern",
         "--repl-port", str(args.repl_port),
         "--ctl-port", str(args.ctl_port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = False
    try:
        time.sleep(args.boot_wait)
        if proc.poll() is not None:
            sys.exit(f"smoke_camera: halo-emu exited early "
                     f"({proc.returncode})")

        ctl = Ctl(args.ctl_port)
        r = Repl(args.repl_port)
        time.sleep(1.0)
        r.send(0, b"\x03")  # break main.lua so its output stays quiet
        time.sleep(0.5)
        r.drain()

        check("ctl ping", ctl.cmd("ping") == "ok")

        # 1. nothing has touched the camera yet
        got = fields(ctl.cmd("camera?"))
        check("camera idle at boot",
              got["source"] == "file" and got["captures"] == "0"
              and got["triggers"] == "0" and got["size"] == "0x0",
              str(got))

        # 2. wake the service and capture
        check("power_save(false)",
              r.lua("frame.camera.power_save(false) print('woke')") == "woke")
        time.sleep(1.0)
        check("capture completes", capture(r))
        got = fields(ctl.cmd("camera?"))
        check("one capture is 3 DMA'd frames",
              got["captures"] == str(SKIP_FRAMES), str(got))
        check("the firmware triggered the PAG7982 over I2C",
              int(got["triggers"]) >= 1, str(got))
        check("the guest programmed the sensor geometry",
              got["size"] == f"{WIDTH}x{HEIGHT}", str(got))

        # 3. the raw frame is exactly the injected image
        want = cs.bayer_bggr8(cs.test_pattern(), WIDTH, HEIGHT)[:ROW]
        raw = fetch(r, f"frame.camera.read_raw({ROW})", ROW)
        check("read_raw() returns the injected test pattern byte for byte",
              raw == want,
              f"got {list(raw[:8])}... want {list(want[:8])}..."
              if raw != want else f"{len(raw)} bytes")

        # 4. the firmware's own JPEG decodes back to the same bars
        jpeg = fetch(r, "frame.camera.read(480)", 200000)
        check("frame.camera.read() yields a JPEG",
              jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9",
              f"{len(jpeg)} bytes, {jpeg[:2].hex()}..{jpeg[-2:].hex()}")
        jw, jh, jrgb = cs.decode_jpeg(jpeg)
        check("the JPEG is the sensor's frame size",
              (jw, jh) == (WIDTH, HEIGHT), f"{jw}x{jh}")
        bad = []
        for bar in range(8):
            x = bar * WIDTH // 8 + WIDTH // 16
            want_sig = tuple(v > 128 for v in cs.bar_colour_at(x))
            got_sig = signature(jrgb, jw, x, HEIGHT // 4)
            if got_sig != want_sig:
                bad.append((x, got_sig, want_sig))
        check("all eight colour bars survive debayer + JPEG",
              not bad, str(bad))

        # 5. swap the source at runtime, from both decoders
        for label, path in (("png", png_path), ("ppm", ppm_path)):
            reply = ctl.cmd(f"camera {path}")
            check(f"camera <{label}> accepted",
                  reply.startswith(f"ok {WIDTH}x{HEIGHT}"), reply)
            check(f"capture after the {label} swap", capture(r))
            want = cs.bayer_bggr8(quad, WIDTH, HEIGHT)[:ROW]
            raw = fetch(r, f"frame.camera.read_raw({ROW})", ROW)
            check(f"read_raw() returns the swapped {label} image",
                  raw == want, f"got {list(raw[:6])}, want {list(want[:6])}")
            quad_jpeg = fetch(r, "frame.camera.read(480)", 200000)

        # 6. no source at all: the model's built-in gradient
        check("camera none", ctl.cmd("camera none") == "ok")
        got = fields(ctl.cmd("camera?"))
        check("source falls back to the gradient",
              got["source"] == "gradient", str(got))
        check("capture on the gradient", capture(r))
        raw = fetch(r, f"frame.camera.read_raw({ROW})", ROW)
        # Row 0 of the gradient: red = x*255/(w-1), green = 0 (y = 0),
        # blue = (x ^ y) & 0xff.  Even columns carry B, odd columns G.
        want = bytes((x & 0xFF) if not (x & 1) else 0 for x in range(ROW))
        check("read_raw() returns the built-in gradient",
              raw == want, f"got {list(raw[:10])}, want {list(want[:10])}")

        # 7. a multi-frame source: the firmware's own two JPEGs, back in
        # as an MJPEG.  The capture delivers SKIP_FRAMES frames, so with
        # two source frames it walks 0, 1, 0 and keeps the last one.
        mjpeg_path = os.path.join(workdir, "pair.mjpeg")
        open(mjpeg_path, "wb").write(jpeg + quad_jpeg)
        reply = ctl.cmd(f"camera {mjpeg_path}")
        check("camera <mjpeg> accepted",
              reply == f"ok {WIDTH}x{HEIGHT} frames=2", reply)
        _, _, mframes = cs.load(mjpeg_path)
        check("capture on the mjpeg", capture(r))
        got = fields(ctl.cmd("camera?"))
        check("the mjpeg source advances one frame per delivered frame",
              got["frames"] == "2"
              and got["frame"] == str(SKIP_FRAMES % 2), str(got))
        want = cs.bayer_bggr8(mframes[0], WIDTH, HEIGHT)[:ROW]
        raw = fetch(r, f"frame.camera.read_raw({ROW})", ROW)
        check("read_raw() returns the mjpeg frame the walk landed on",
              raw == want, f"got {list(raw[:6])}, want {list(want[:6])}")

        # 8. a broken source is refused and leaves the running one alone
        ctl.cmd(f"camera {png_path}")
        reply = raw_cmd(ctl, f"camera {bad_path}")
        check("a broken image is refused", reply.startswith("err"), reply)
        check("the refusal left the running source in place",
              fields(ctl.cmd("camera?"))["source"] == "file")
        check("the guest is still alive",
              r.lua("print('alive')") == "alive")

        print("smoke_camera: PASS — all checks green")
        ok = True
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        for name in os.listdir(workdir):
            os.unlink(os.path.join(workdir, name))
        os.rmdir(workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
