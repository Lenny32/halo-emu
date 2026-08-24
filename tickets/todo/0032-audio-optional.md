# 0032 — Audio out/in (optional)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028 (LC3 ROM entry points currently return not-supported)
**Effort:** L

## Story

Speaker and microphone emulation for the audio features (startup sound, Lua audio APIs,
mic capture). Optional: nothing on the boot path hard-depends on audio (`main.c`'s startup
sound wait is bounded — "a missing/stuck speaker can't stall boot").

## Tasks

1. **Speaker path**: `i2s0@49014000` (`alif,i2s-sync`, IRQ 141 — note the firmware's ISR
   lives in DTCM `.ramfunc`; driver `alif/drivers/i2s/i2s_sync/i2s_sync.c`, 16-bit 32 kHz
   mono) + MAX98357A enable GPIO (gpio8.5). Model the I2S FIFO/IRQ cadence and sink
   samples to host audio (QEMU audiodev) or `--wav-out <file>`. Gate the startup sound.
2. **Mic path**: LPPDM `pdm_audio@43002000` (IRQ 49, `alif,t5838-alif-pdm`, DMA `dma2`
   ch4 req 30 — instantiate QEMU's PL330 @ `0x400C0000`, IRQs 0-4/32). Feed PDM/PCM data
   from `--wav-in <file>` or host capture. Lazy-init: only exercised by
   `frame.microphone.*`.
3. **LC3**: revisit the 0028 stub's LC3 entry points — implement passthrough/decode via a
   host-side LC3 library (liblc3) if BLE-audio tests need it; otherwise keep
   not-supported.

## Gate (acceptance)

- Startup sound audible (or lands in `--wav-out`) on boot.
- `frame.microphone` Lua capture returns the injected samples via the REPL.
