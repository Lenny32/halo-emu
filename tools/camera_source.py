#!/usr/bin/env python3
"""camera_source.py — camera frame sources for the halo emulator
(ticket 0033).

The QEMU LPCAM model (`patches/files/hw/arm/halo_lpcam.c`) reads frames
from a trivial container rather than decoding images itself: the pinned
QEMU build has neither libpng nor libjpeg, so all image handling lives
here instead.  `halo-emu --camera <src>` and the control socket's
`camera <src>` verb both go through `build_container()`.

Container layout (little-endian, matching HALOCAM1 in halo_lpcam.c):

    0x00  b"HALOCAM1"
    0x08  uint32 width, height, frames, interval_ms
    0x18  frames x width x height x 3 bytes, RGB888, top-down

`interval_ms` = 0 means "use the sensor's nominal rate" (30 fps).

Sources, all decoded with the standard library only:

    test-pattern   SMPTE-ish colour bars over a grey ramp, generated here
    .png           8/16-bit greyscale, RGB, RGBA and palette, uninterlaced
    .pgm .ppm      binary PNM (P5/P6)
    .jpg .jpeg     baseline (sequential DCT) JPEG, greyscale or YCbCr
    .mjpeg .mjpg   concatenated baseline JPEGs, one per frame

The baseline JPEG decoder also serves the smoke test in the other
direction: the firmware hands back a JPEG from `frame.camera.read()`,
and `decode_jpeg()` is what turns it into pixels to compare against the
image that was injected.  It is pure Python and therefore slow (a few
seconds for 640x480); `max_frames` bounds what an MJPEG costs.

Unrecognised extensions are sniffed by magic number, so `--camera
frame.dat` still works if the content is a PNG or a JPEG.
"""

import math
import os
import struct
import sys
import zlib

MAGIC = b"HALOCAM1"
HDR_LEN = 24

# The sensor's only supported format (pag7982.c fmts[]), and therefore
# the natural size to hand the model: anything else is scaled by the
# LPCAM model with nearest-neighbour sampling.
SENSOR_WIDTH = 640
SENSOR_HEIGHT = 480


class CameraSourceError(Exception):
    """Bad or unsupported image source."""


# ---------------------------------------------------------------------
# Container
# ---------------------------------------------------------------------

def write_container(path, width, height, frames, interval_ms=0):
    """Write the HALOCAM1 container the LPCAM model reads."""
    if not frames:
        raise CameraSourceError("no frames to write")
    expect = width * height * 3
    for i, f in enumerate(frames):
        if len(f) != expect:
            raise CameraSourceError(
                f"frame {i} is {len(f)} bytes, expected {expect} "
                f"({width}x{height} RGB888)")
    with open(path, "wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack("<IIII", width, height, len(frames),
                             interval_ms))
        for f in frames:
            fh.write(f)
    return path


def build_container(spec, path, max_frames=30, interval_ms=0):
    """Decode `spec` and write it to `path`; returns (w, h, nframes)."""
    width, height, frames = load(spec, max_frames=max_frames)
    write_container(path, width, height, frames, interval_ms)
    return width, height, len(frames)


def load(spec, max_frames=30):
    """Decode a source spec into (width, height, [rgb888 frame, ...])."""
    if spec == "test-pattern":
        return SENSOR_WIDTH, SENSOR_HEIGHT, [test_pattern()]

    try:
        data = open(spec, "rb").read()
    except OSError as e:
        raise CameraSourceError(f"cannot read {spec}: {e}") from e

    kind = _sniff(spec, data)
    if kind == "png":
        w, h, rgb = decode_png(data)
        return w, h, [rgb]
    if kind == "pnm":
        w, h, rgb = decode_pnm(data)
        return w, h, [rgb]
    if kind == "jpeg":
        chunks = split_mjpeg(data)
        if len(chunks) > max_frames:
            print(f"camera_source: {spec} has {len(chunks)} frames, using "
                  f"the first {max_frames} (--camera-max-frames)",
                  file=sys.stderr)
            chunks = chunks[:max_frames]
        frames = []
        size = None
        for i, chunk in enumerate(chunks):
            w, h, rgb = decode_jpeg(chunk)
            if size is None:
                size = (w, h)
            elif (w, h) != size:
                raise CameraSourceError(
                    f"{spec} frame {i} is {w}x{h}, frame 0 was "
                    f"{size[0]}x{size[1]}: mixed sizes are not supported")
            frames.append(rgb)
        return size[0], size[1], frames
    raise CameraSourceError(
        f"{spec}: unsupported image format (want png, pnm/ppm/pgm, "
        f"baseline jpeg/mjpeg, or the literal 'test-pattern')")


def _sniff(spec, data):
    ext = os.path.splitext(spec)[1].lower()
    if ext == ".png" or data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if ext in (".pnm", ".ppm", ".pgm") or data[:2] in (b"P5", b"P6"):
        return "pnm"
    if ext in (".jpg", ".jpeg", ".mjpeg", ".mjpg") or data[:2] == b"\xff\xd8":
        return "jpeg"
    return None


# ---------------------------------------------------------------------
# Test pattern
# ---------------------------------------------------------------------

BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def test_pattern(width=SENSOR_WIDTH, height=SENSOR_HEIGHT):
    """Eight colour bars over the top two thirds, a grey ramp below.

    Chosen so a test can assert exact colours: every bar is a saturated
    primary, and the ramp gives a monotone reference for the debayer +
    JPEG round trip.
    """
    split = height * 2 // 3
    rows = []
    bar_row = bytearray()
    for x in range(width):
        bar_row += bytes(BARS[min(x * len(BARS) // width, len(BARS) - 1)])
    ramp_row = bytearray()
    for x in range(width):
        v = x * 255 // max(1, width - 1)
        ramp_row += bytes((v, v, v))
    for y in range(height):
        rows.append(bytes(bar_row if y < split else ramp_row))
    return b"".join(rows)


def bar_colour_at(x, width=SENSOR_WIDTH):
    """The colour bar covering column `x` — the test's expected value."""
    return BARS[min(x * len(BARS) // width, len(BARS) - 1)]


# ---------------------------------------------------------------------
# Bayer helpers (shared with the LPCAM model's mosaic)
# ---------------------------------------------------------------------

def bayer_bggr8(rgb, width, height):
    """Mosaic an RGB888 frame the way the LPCAM model does.

    even rows  B G B G ...
    odd rows   G R G R ...
    """
    out = bytearray(width * height)
    for y in range(height):
        base = y * width
        src = base * 3
        if y & 1:
            chans = (1, 0)  # G, R
        else:
            chans = (2, 1)  # B, G
        for x in range(width):
            out[base + x] = rgb[src + x * 3 + chans[x & 1]]
    return bytes(out)


# ---------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------

def decode_png(data):
    """Decode an uninterlaced 8/16-bit PNG to (w, h, rgb888)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise CameraSourceError("not a PNG file")
    pos, ihdr, plte, idat = 8, None, b"", []
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + crc
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if ihdr is None or not idat:
        raise CameraSourceError("PNG has no IHDR/IDAT")

    width, height, depth, colour, comp, filt, interlace = ihdr
    if comp != 0 or filt != 0:
        raise CameraSourceError("PNG uses a non-standard compression/filter")
    if interlace:
        raise CameraSourceError("interlaced (Adam7) PNG is not supported")
    if depth not in (8, 16):
        raise CameraSourceError(f"PNG bit depth {depth} is not supported "
                                "(want 8 or 16)")
    nchan = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour)
    if nchan is None:
        raise CameraSourceError(f"PNG colour type {colour} is not supported")
    if colour == 3 and depth != 8:
        raise CameraSourceError("palette PNG must be 8-bit")

    bpp = nchan * depth // 8
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat))
    if len(raw) < (stride + 1) * height:
        raise CameraSourceError("PNG image data is truncated")

    rows = _png_unfilter(raw, width, height, stride, bpp)
    return width, height, _png_to_rgb(rows, width, height, depth, colour,
                                      nchan, plte)


def _png_unfilter(raw, width, height, stride, bpp):
    prev = bytearray(stride)
    rows = []
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise CameraSourceError(f"PNG row filter {ftype} is invalid")
        rows.append(line)
        prev = line
    return rows


def _png_to_rgb(rows, width, height, depth, colour, nchan, plte):
    step = depth // 8          # bytes per sample; 16-bit keeps the MSB
    out = bytearray(width * height * 3)
    o = 0
    for y in range(height):
        line = rows[y]
        for x in range(width):
            base = x * nchan * step
            if colour == 3:
                idx = line[base] * 3
                if idx + 3 > len(plte):
                    raise CameraSourceError("PNG palette index out of range")
                out[o:o + 3] = plte[idx:idx + 3]
            elif colour in (0, 4):
                g = line[base]
                out[o] = out[o + 1] = out[o + 2] = g
            else:
                out[o] = line[base]
                out[o + 1] = line[base + step]
                out[o + 2] = line[base + 2 * step]
            o += 3
    return bytes(out)


def encode_png(rgb, width, height):
    """Encode an RGB888 frame as an uninterlaced 8-bit RGB PNG.

    The inverse of decode_png, for tests that need a real PNG on disk
    and for the CLI's --out foo.png.
    """
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(ctype, body):
        return (struct.pack(">I", len(body)) + ctype + body +
                struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) +
            chunk(b"IEND", b""))


# ---------------------------------------------------------------------
# PNM
# ---------------------------------------------------------------------

def decode_pnm(data):
    """Decode binary PGM (P5) / PPM (P6) to (w, h, rgb888)."""
    magic = data[:2]
    if magic not in (b"P5", b"P6"):
        raise CameraSourceError("not a binary PGM/PPM (want P5 or P6)")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # the single whitespace byte after maxval
    width, height, maxval = fields
    if maxval != 255:
        raise CameraSourceError(f"PNM maxval {maxval} is not supported "
                                "(want 255)")
    nchan = 1 if magic == b"P5" else 3
    need = width * height * nchan
    body = data[pos:pos + need]
    if len(body) < need:
        raise CameraSourceError("PNM image data is truncated")
    if nchan == 3:
        return width, height, body
    out = bytearray(width * height * 3)
    for i, g in enumerate(body):
        out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = g
    return width, height, bytes(out)


# ---------------------------------------------------------------------
# Baseline JPEG
# ---------------------------------------------------------------------

ZIGZAG = [
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]

# 1-D IDCT basis: out[x] = sum_u COS[x][u] * F[u]
_C0 = 1.0 / math.sqrt(8.0)
COS = [[(_C0 if u == 0 else 0.5 * math.cos((2 * x + 1) * u * math.pi / 16))
        for u in range(8)] for x in range(8)]


def split_mjpeg(data):
    """Split concatenated JPEGs into frames.

    Splits only at an SOI that starts the stream or directly follows an
    EOI, so a JPEG thumbnail embedded in an APPn segment does not look
    like a frame boundary.
    """
    if data[:2] != b"\xff\xd8":
        raise CameraSourceError("not a JPEG (no SOI)")
    frames, start, pos = [], 0, 2
    while True:
        end = data.find(b"\xff\xd9", pos)
        if end < 0:
            frames.append(data[start:])
            break
        frames.append(data[start:end + 2])
        pos = end + 2
        if data[pos:pos + 2] != b"\xff\xd8":
            break
        start = pos
        pos += 2
    return frames


class _Component:
    __slots__ = ("cid", "h", "v", "tq", "td", "ta", "bw", "bh", "plane",
                 "stride", "pred")


class _BitReader:
    """MSB-first entropy-coded bit reader with byte de-stuffing."""

    def __init__(self, data, pos):
        self.d = data
        self.p = pos
        self.acc = 0
        self.n = 0

    def bit(self):
        if self.n == 0:
            if self.p >= len(self.d):
                return 0
            byte = self.d[self.p]
            self.p += 1
            if byte == 0xFF:
                nxt = self.d[self.p] if self.p < len(self.d) else 0xD9
                if nxt == 0x00:
                    self.p += 1
                else:
                    # A real marker: stop consuming and feed zero bits,
                    # which is what a decoder does at a truncated scan.
                    self.p -= 1
                    return 0
            self.acc = byte
            self.n = 8
        self.n -= 1
        return (self.acc >> self.n) & 1

    def bits(self, count):
        v = 0
        for _ in range(count):
            v = (v << 1) | self.bit()
        return v

    def align(self):
        self.n = 0

    def skip_rst(self):
        """Step over an RSTn marker at the current byte position."""
        self.align()
        while self.p + 1 < len(self.d):
            if self.d[self.p] == 0xFF and 0xD0 <= self.d[self.p + 1] <= 0xD7:
                self.p += 2
                return True
            self.p += 1
        return False


def _build_huffman(counts, values):
    """jpeg-6b style (mincode, maxcode, valptr, values) tables."""
    mincode = [0] * 17
    maxcode = [-1] * 17
    valptr = [0] * 17
    code = k = 0
    for length in range(1, 17):
        valptr[length] = k
        mincode[length] = code
        code += counts[length - 1]
        k += counts[length - 1]
        maxcode[length] = code - 1 if counts[length - 1] else -1
        code <<= 1
    return mincode, maxcode, valptr, values


def _huff_decode(br, table):
    mincode, maxcode, valptr, values = table
    code = br.bit()
    length = 1
    while length < 17 and (maxcode[length] < 0 or code > maxcode[length]):
        code = (code << 1) | br.bit()
        length += 1
    if length > 16:
        return 0
    idx = valptr[length] + code - mincode[length]
    return values[idx] if idx < len(values) else 0


def _extend(value, nbits):
    if nbits and value < (1 << (nbits - 1)):
        return value - (1 << nbits) + 1
    return value


def _idct_block(coef, plane, stride, px, py):
    """8x8 IDCT of dequantised `coef` into `plane` at (px, py)."""
    tmp = [0.0] * 64
    for i in range(8):
        o = i * 8
        c0, c1, c2, c3 = coef[o], coef[o + 1], coef[o + 2], coef[o + 3]
        c4, c5, c6, c7 = coef[o + 4], coef[o + 5], coef[o + 6], coef[o + 7]
        if not (c1 or c2 or c3 or c4 or c5 or c6 or c7):
            # AC-free row: the whole row is the flat DC term.  Very
            # common, and skipping the 64 multiplies here is most of
            # what makes a pure-Python IDCT tolerable.
            v = c0 * _C0
            tmp[o] = tmp[o + 1] = tmp[o + 2] = tmp[o + 3] = v
            tmp[o + 4] = tmp[o + 5] = tmp[o + 6] = tmp[o + 7] = v
            continue
        for x in range(8):
            k = COS[x]
            tmp[o + x] = (k[0] * c0 + k[1] * c1 + k[2] * c2 + k[3] * c3 +
                          k[4] * c4 + k[5] * c5 + k[6] * c6 + k[7] * c7)

    for x in range(8):
        c0, c1, c2, c3 = tmp[x], tmp[8 + x], tmp[16 + x], tmp[24 + x]
        c4, c5, c6, c7 = tmp[32 + x], tmp[40 + x], tmp[48 + x], tmp[56 + x]
        if not (c1 or c2 or c3 or c4 or c5 or c6 or c7):
            v = int(c0 * _C0 + 128.5)
            v = 0 if v < 0 else (255 if v > 255 else v)
            for y in range(8):
                plane[(py + y) * stride + px + x] = v
            continue
        for y in range(8):
            k = COS[y]
            v = int(k[0] * c0 + k[1] * c1 + k[2] * c2 + k[3] * c3 +
                    k[4] * c4 + k[5] * c5 + k[6] * c6 + k[7] * c7 + 128.5)
            plane[(py + y) * stride + px + x] = (
                0 if v < 0 else (255 if v > 255 else v))


def decode_jpeg(data):
    """Decode a baseline (SOF0/SOF1) JPEG to (w, h, rgb888)."""
    if data[:2] != b"\xff\xd8":
        raise CameraSourceError("not a JPEG (no SOI)")

    qt = {}
    hdc, hac = {}, {}
    comps = []
    width = height = 0
    restart_interval = 0
    pos = 2

    while pos + 4 <= len(data):
        if data[pos] != 0xFF:
            pos += 1
            continue
        marker = data[pos + 1]
        pos += 2
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if marker == 0xD9:
            break
        (seglen,) = struct.unpack_from(">H", data, pos)
        seg = data[pos + 2:pos + seglen]
        nxt = pos + seglen

        if marker == 0xDB:                       # DQT
            i = 0
            while i < len(seg):
                pq, tq = seg[i] >> 4, seg[i] & 0xF
                i += 1
                table = [0] * 64
                for k in range(64):
                    if pq:
                        table[ZIGZAG[k]] = struct.unpack_from(">H", seg, i)[0]
                        i += 2
                    else:
                        table[ZIGZAG[k]] = seg[i]
                        i += 1
                qt[tq] = table
        elif marker == 0xC4:                     # DHT
            i = 0
            while i < len(seg):
                tc, th = seg[i] >> 4, seg[i] & 0xF
                counts = list(seg[i + 1:i + 17])
                total = sum(counts)
                values = list(seg[i + 17:i + 17 + total])
                i += 17 + total
                (hac if tc else hdc)[th] = _build_huffman(counts, values)
        elif marker in (0xC0, 0xC1):             # SOF0/SOF1 baseline
            height, width = struct.unpack_from(">HH", seg, 1)
            ncomp = seg[5]
            comps = []
            for c in range(ncomp):
                base = 6 + c * 3
                comp = _Component()
                comp.cid = seg[base]
                comp.h = seg[base + 1] >> 4
                comp.v = seg[base + 1] & 0xF
                comp.tq = seg[base + 2]
                comps.append(comp)
        elif marker == 0xC2:
            raise CameraSourceError("progressive JPEG is not supported "
                                    "(only baseline SOF0/SOF1)")
        elif marker in (0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB,
                        0xCD, 0xCE, 0xCF):
            raise CameraSourceError(f"JPEG SOF type 0x{marker:02X} is not "
                                    "supported (only baseline SOF0/SOF1)")
        elif marker == 0xDD:                     # DRI
            (restart_interval,) = struct.unpack_from(">H", seg, 0)
        elif marker == 0xDA:                     # SOS
            ns = seg[0]
            scan = []
            for c in range(ns):
                cid, tables = seg[1 + c * 2], seg[2 + c * 2]
                comp = next((k for k in comps if k.cid == cid), None)
                if comp is None:
                    raise CameraSourceError("SOS names an unknown component")
                comp.td = tables >> 4
                comp.ta = tables & 0xF
                scan.append(comp)
            if len(scan) != len(comps):
                raise CameraSourceError("multi-scan JPEG is not supported")
            return _decode_scan(data, nxt, width, height, comps, qt,
                                hdc, hac, restart_interval)
        pos = nxt

    raise CameraSourceError("JPEG has no baseline scan")


def _decode_scan(data, pos, width, height, comps, qt, hdc, hac, ri):
    if not width or not height or not comps:
        raise CameraSourceError("JPEG has no frame header before the scan")
    if len(comps) not in (1, 3):
        raise CameraSourceError(f"{len(comps)}-component JPEG is not "
                                "supported (want greyscale or YCbCr)")

    hmax = max(c.h for c in comps)
    vmax = max(c.v for c in comps)
    mcus_x = (width + 8 * hmax - 1) // (8 * hmax)
    mcus_y = (height + 8 * vmax - 1) // (8 * vmax)

    for c in comps:
        c.bw = mcus_x * c.h
        c.bh = mcus_y * c.v
        c.stride = c.bw * 8
        c.plane = bytearray(c.stride * c.bh * 8)
        c.pred = 0
        if c.tq not in qt:
            raise CameraSourceError(f"JPEG references missing DQT {c.tq}")

    br = _BitReader(data, pos)
    coef = [0.0] * 64
    mcu = 0
    total_mcus = mcus_x * mcus_y

    while mcu < total_mcus:
        if ri and mcu and mcu % ri == 0:
            if not br.skip_rst():
                break
            for c in comps:
                c.pred = 0
        my, mx = divmod(mcu, mcus_x)
        for c in comps:
            quant = qt[c.tq]
            dc_tbl = hdc.get(c.td)
            ac_tbl = hac.get(c.ta)
            if dc_tbl is None or ac_tbl is None:
                raise CameraSourceError("JPEG references a missing DHT")
            for by in range(c.v):
                for bx in range(c.h):
                    for i in range(64):
                        coef[i] = 0.0
                    t = _huff_decode(br, dc_tbl)
                    c.pred += _extend(br.bits(t), t) if t else 0
                    coef[0] = c.pred * quant[0]
                    k = 1
                    while k < 64:
                        rs = _huff_decode(br, ac_tbl)
                        r, s = rs >> 4, rs & 0xF
                        if s == 0:
                            if r != 15:
                                break        # EOB
                            k += 16
                            continue
                        k += r
                        if k > 63:
                            break
                        z = ZIGZAG[k]
                        coef[z] = _extend(br.bits(s), s) * quant[z]
                        k += 1
                    _idct_block(coef, c.plane, c.stride,
                                (mx * c.h + bx) * 8, (my * c.v + by) * 8)
        mcu += 1

    return width, height, _to_rgb(comps, width, height, hmax, vmax)


def _to_rgb(comps, width, height, hmax, vmax):
    out = bytearray(width * height * 3)
    if len(comps) == 1:
        c = comps[0]
        o = 0
        for y in range(height):
            row = y * c.stride
            plane = c.plane
            for x in range(width):
                g = plane[row + x]
                out[o] = out[o + 1] = out[o + 2] = g
                o += 3
        return bytes(out)

    y_c, cb_c, cr_c = comps
    o = 0
    for y in range(height):
        yrow = (y * y_c.v // vmax) * y_c.stride
        brow = (y * cb_c.v // vmax) * cb_c.stride
        rrow = (y * cr_c.v // vmax) * cr_c.stride
        yp, bp, rp = y_c.plane, cb_c.plane, cr_c.plane
        for x in range(width):
            yy = yp[yrow + (x * y_c.h // hmax)]
            cb = bp[brow + (x * cb_c.h // hmax)] - 128
            cr = rp[rrow + (x * cr_c.h // hmax)] - 128
            r = yy + 1.402 * cr
            g = yy - 0.344136 * cb - 0.714136 * cr
            b = yy + 1.772 * cb
            out[o] = 0 if r < 0 else (255 if r > 255 else int(r))
            out[o + 1] = 0 if g < 0 else (255 if g > 255 else int(g))
            out[o + 2] = 0 if b < 0 else (255 if b > 255 else int(b))
            o += 3
    return bytes(out)


# ---------------------------------------------------------------------
# CLI (handy for eyeballing a source without booting the emulator)
# ---------------------------------------------------------------------

def main(argv):
    import argparse

    p = argparse.ArgumentParser(
        description="Decode a camera source into a HALOCAM1 container "
                    "(or a PPM, for eyeballing).")
    p.add_argument("source", help="test-pattern, or a png/pnm/jpeg/mjpeg")
    p.add_argument("-o", "--out", required=True,
                   help="output file; .ppm/.png writes frame 0 as an "
                        "image, anything else writes the container")
    p.add_argument("--max-frames", type=int, default=30)
    p.add_argument("--interval-ms", type=int, default=0)
    args = p.parse_args(argv)

    try:
        width, height, frames = load(args.source,
                                     max_frames=args.max_frames)
    except CameraSourceError as e:
        sys.exit(f"camera_source: {e}")
    if args.out.lower().endswith(".ppm"):
        with open(args.out, "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (width, height))
            fh.write(frames[0])
    elif args.out.lower().endswith(".png"):
        open(args.out, "wb").write(encode_png(frames[0], width, height))
    else:
        write_container(args.out, width, height, frames, args.interval_ms)
    print(f"camera_source: {args.source} -> {args.out} "
          f"({width}x{height}, {len(frames)} frame(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
