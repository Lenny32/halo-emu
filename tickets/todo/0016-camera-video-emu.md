# 0016 — Fake camera: emu video driver + libmpix pipeline (M5)

**Phase:** 1 — core emulator
**Depends on:** 0009
**Effort:** M/L (scheduled last of the fakes)

## Story

`lua_camera.c` uses the standard Zephyr video API (`video_get_caps/set_format/
buffer_alloc/enqueue/dequeue/stream_start/stop/flush`) plus the pure-software libmpix
pipeline (debayer → AWB → JPEG). Zephyr 3.6's `video_sw_generator` is not
DT-instantiated, so write a small emu video driver serving a test pattern or injected
frames — `lua_camera.c` and the mpix pipeline stay unmodified.

## Tasks

1. `drivers/video/video_emu.c` — `compatible = "halo,emu-video"`, chosen `zephyr,video`
   in `native_sim.overlay`. Implements the video API; serves frames in **the same pixel
   format `pag7982` advertises** (mirror `drivers/video/pag7982.c` caps — likely raw
   Bayer — so the debayer stage stays honest).
2. Frame sources: built-in test pattern (default) and control-plane
   `camera load <path>` (0009) — a raw/Bayer file pushed as the next capture(s).
   Frame-rate pacing per the configured fps (hardware 1–30 fps).
3. Verify `libmpix_init/process` path untouched (`lua_camera.c:129/:156`);
   `frame.camera.capture` → `image_ready` → `read`/`read_raw` (JPEG out over channel 2
   / data channel per PROTOCOL.md).
4. `test_camera.py` subset green via pyshim (capture + JPEG decode host-side).

## Key points in code

- `modules/halo/src/lua_camera.c:571` — `DT_CHOSEN(zephyr_video)` grab; `:564`
  `camera_hardware_init` sequence the driver must satisfy
- `drivers/video/pag7982.c` + `dts/bindings/pixart,pga7982.yaml` — format/caps/fps
  reference
- `modules/lib/libmpix` (workspace) — pure software, no changes

## Acceptance criteria

- [ ] `frame.camera.capture()` + `read` returns a decodable JPEG of the test pattern
- [ ] Injected file frames come back correctly (visual check)
- [ ] Camera test subset green
