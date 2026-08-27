# 0033 — Camera (optional)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028
**Effort:** M

## Story

Emulate the camera so `frame.camera.*` Lua APIs work: LPCAM controller fed from host-side
image/video files. Lazy-init and graceful on failure — purely additive.

## Tasks

1. **LPCAM controller** `lpcam@43003000` (`alif,cam`, IRQ 54; driver
   `zephyr/drivers/video/video_alif.c`): **parallel 8-bit interface, not CSI**, inverted
   H/V sync; DMA'd capture into firmware buffers — model frame delivery + IRQ.
2. **PAG7982 sensor** as an I2C1 target @ `0x40` (driver `alif/drivers/video/pag7982.c`):
   ack the register init sequence (reset via gpio1.4, `cam_1v8` regulator handled by the
   GPIO/regulator models); 24 MHz pixclk, 30 fps nominal.
3. Source: `--camera <png/jpg/mjpeg-file|test-pattern>`, converted to the sensor's output
   format; frame-rate pacing.

## Gate (acceptance)

- `frame.camera` capture via the REPL returns the injected test image.
- Boot and all other tickets' gates remain unaffected with no `--camera` given.

## Outcome (done 2026-08-27)

Two new models, one new tool module, one new gate. Purely additive: with no `--camera` the
boot log is byte-for-byte what it was, and all six existing smoke tests still pass.

1. **`patches/files/hw/arm/halo_lpcam.c`** — LPCAM `0x43003000` / IRQ 54. `START` in
   `CAM_CTRL` DMAs one frame to `CAM_FRAME_ADDR` and raises `VSYNC | STOP`; `BUSY` is
   read-only and held for the frame period (`cam_stream_start` refuses to start on it,
   `cam_stream_stop` and `cam_flush` poll it down); `SW_RESET` aborts and clears the
   status. One frame per `START` is all that is needed even though the devicetree leaves
   `capture-mode` at its "snapshot" default, because `cam_work_helper` re-arms per frame
   from the STOP interrupt either way.
2. **`patches/files/hw/arm/halo_pag7982.c`** — the sensor on I2C1 `0x40`: a bank-switched
   register file (`BANK_SEL` 0xEF) whose only special values are `PART_ID_L/H` = 0x82/0x79
   in bank 0. Those had to be **read-only**: the 160-entry `default_regs` sequence writes
   registers 0x00/0x01 in banks 2 and 4, so a bank-agnostic register file clobbers the ID
   and `pag7982_check_connection()` fails the whole resume.
3. **`tools/camera_source.py`** — `--camera <test-pattern|png|pnm|jpeg|mjpeg>`, plus the
   `camera` control-socket verb for swapping the source at runtime, and `camera?` for the
   readouts. Machine properties `camera-file/-source/-frames/-frame/-captures/-width/
   -height/-streaming/-triggers`.

### Decisions worth keeping

- **All image decoding is host-side, in Python.** The pinned QEMU build has neither
  libpng nor libjpeg (`qemu/build/config-host.h`: `#undef CONFIG_PNG`, `#undef
  CONFIG_VNC_JPEG`) and the host has no Pillow/numpy/ffmpeg, so the model reads a trivial
  raw `HALOCAM1` container (magic, w/h/frames/interval, then RGB888 frames) and the
  launcher writes it. That includes a **baseline JPEG decoder** written for this ticket —
  stdlib only, like the rest of the launcher. It costs ~0.5 s per VGA frame, which is fine
  for a still and is why MJPEG has a `--camera-max-frames` cap (default 30). Progressive
  JPEG is refused with a clear message rather than decoded wrongly.
- **The parallel pixel bus is not modelled.** Nothing guest-side can observe it, so the
  controller synthesises what the sensor would have driven rather than pretending to clock
  bytes between two models. The sensor model therefore has no pixel path at all — it only
  has to ack its init sequence.
- **The sensor's format is Bayer, not RGB.** `pag7982.c fmts[]` has exactly one entry,
  `VIDEO_PIX_FMT_BGGR8` at 640x480, so the model mosaics its RGB source to one byte per
  pixel (even rows `B G`, odd rows `G R`) and the firmware's libmpix does the debayer and
  the JPEG encode. Getting the phase wrong would have been invisible in a screenshot and
  obvious only as swapped colour channels, which is why the gate asserts the raw bytes.
- **Multi-frame sources advance per delivered frame, not per wall-clock tick.** A run is
  then reproducible: `capture()` always walks the same frames in the same order. At the
  30 fps the sensor is configured for that is also real time; `--camera-fps` overrides the
  pacing.
- **Frame delivery is not gated on the sensor being streaming.** It was tempting (the
  model knows `R_TRG_EN`), but a wrong reading of the trigger sequence would hang the
  guest instead of failing it. Instead `R_TRG_EN` is exposed as `camera-streaming` plus a
  latching `camera-triggers` count, so a test can *assert* the firmware started the sensor
  without the model being able to deadlock on it. `camera-triggers` exists because
  `camera-streaming` is only true *during* a capture and polling it races the guest.
- **One Lua `capture()` is three DMA'd frames**, not four: `LUA_CAMERA_SKIP_FRAMES` is 3
  and the loop keeps the last of the three rather than taking a fourth.
- `reset-gpios` (gpio1.4) and the `cam_1v8` rail are not modelled — `halo_gpio.c` has no
  output lines to observe, and the driver never reads anything back that depends on them.
  Harmless: the init sequence is idempotent and re-runs in full on every PM resume.

### Gate evidence

- **`tests/smoke_camera.py`: 26/26 green**, against the real `0.8.8.bin`:
  - camera idle at boot (`captures=0 triggers=0 size=0x0`) — the service starts in power
    save, so nothing touches the sensor until Lua asks;
  - `power_save(false)` + `capture()` completes, `captures=3`, `triggers=1`,
    `size=640x480`;
  - **`read_raw()` returns the injected test pattern byte for byte** — 640 bytes compared
    against `bayer_bggr8(test_pattern())`, which is the decisive check;
  - `frame.camera.read()` yields a well-formed 24 KB JPEG (`ffd8`..`ffd9`) that
    `decode_jpeg()` reads back at 640x480 with all eight colour bars carrying the right
    channels high. The assertion is the high/low signature rather than absolute values,
    because libmpix applies automatic black-level and white-balance correction (measured
    ~243 instead of 255, and blue pushed to ~254 by the 1.8x blue balance);
  - runtime `camera <file>` swap verified for **both** PNG and PPM, each followed by a
    fresh capture whose raw bytes match exactly;
  - `camera none` falls back to the built-in gradient and its raw row matches the
    generator formula exactly;
  - **MJPEG**, built by concatenating the two JPEGs the firmware itself produced (no JPEG
    encoder needed on this side): `frames=2`, the frame index advances to
    `SKIP_FRAMES % 2`, and the raw bytes match the frame the walk landed on;
  - a broken image is refused (`err PNG has no IHDR/IDAT`) without disturbing the running
    source, and the REPL still answers.
- **Second half of the gate — nothing else moved.** `smoke_display`, `smoke_ble`,
  `smoke_controls`, `smoke_sensors`, `smoke_audio`, `smoke_le_audio` all pass, run in
  sequence, with **0 orphaned QEMU processes and 0 leaked temp dirs**. A plain
  `./halo-emu -f 0.8.8.bin --headless --screenshot` with no `--camera` logs only the
  expected first-boot littlefs format and renders the boot splash.
- Boot-time sources spot-checked by hand: a real 640x480 JPEG, a 3-frame MJPEG with
  `--camera-fps 10`, `--camera-max-frames 2` (which says what it dropped), and both error
  paths (`cannot read ...`, `unsupported image format ...`).
- The JPEG decoder was validated independently before the emulator ran: four unrelated
  JPEGs on this host (4:2:0 and 4:2:2, 400x600 to 2595x805) decode at the right size, and
  one was rendered to PNG and compared against the original image side by side.
- PNG coverage is unit-checked in both directions: `encode_png`/`decode_png` round-trip
  exactly for 8- and 16-bit RGB, greyscale, RGBA and palette, and for all five row
  filters.

## Not done

- **Progressive JPEG** (SOF2) and interlaced (Adam7) PNG are refused rather than decoded.
  Both would be pure decoder work with no emulator content; the error names the supported
  formats.
- **No `hflip`/`vflip` effect.** `VIDEO_CID_HFLIP`/`VFLIP` reach the sensor's `R_FLIP`
  register and are stored, but the LPCAM model does not mirror the frame. The Lua camera
  API does not expose the controls, so nothing on the device path can reach them today.
- **`CAM_CFG` data modes above 8-bit** log `LOG_UNIMP` once and keep serving 8-bit Bayer
  replicated across the wider pixel. The board wires 8 bits, so nothing asks.
- **No AXI error injection**: `CAM_AXI_ERR_STAT` reads 0 and `INTR_BRESP_ERR` /
  `INTR_*FIFO_OVERRUN` are never raised, so the driver's frame-corruption path
  (`is_not_corrupted_frame`) is unexercised. Worth a follow-up if the firmware's error
  handling ever needs a gate.
