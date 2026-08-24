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
