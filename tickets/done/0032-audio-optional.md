# 0032 — Audio out/in

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028 (LC3 ROM entry points currently return not-supported)
**Effort:** L
**Status:** done

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

## Outcome

Both gates pass; `tests/smoke_audio.py` is the regression (20 checks, all green against
`0.8.8.bin`). Three deviations from the text above are worth recording.

**1. The speaker path is interrupt/FIFO driven, not DMA.** The ticket implies modelling a
DMA cadence, but `boards/arm/halo/halo.dts:123` gives `i2s0` no `dmas` property, so
`i2s_sync.c` takes the ISR path: `i2s_sync_tx_isr_handler()` writes exactly
`I2S_FIFO_TRG_LEVEL` (8) samples per ISR entry. `halo_i2s.c` therefore models a 16-deep TX
FIFO with `ISR.TXFE` as a *level* on the NVIC line, so the guest ISR re-enters until the
FIFO is above the trigger — the hardware behaviour the driver is written against.

One hazard is worth its own note, because it cost a wedged boot before it was found:
**TXFE must not assert until the block has actually clocked a sample out.** `i2s_send()`
sets `dev_data->tx.running = true` *after* `i2s_transmitter_start()` unmasks the
interrupt, and the ISR early-returns while `tx.running` is false. A level IRQ raised in
that window re-enters forever and the guest never advances. The model gates TXFE on a
`tx_started` flag set by the first drain tick.

**2. The mic path needs the PL330 for real, and its FIFO port is read in 16-bit beats.**
`t5838_alif_pdm.c` takes `dma_dev` from the devicetree at compile time and has no
`device_is_ready` fallback, so the ISR drain path is unreachable on this board and QEMU's
upstream `pl330` had to be instantiated. It works with Zephyr's `dma_pl330.c` unmodified.
Two details mattered:

- QEMU's PL330 peripheral-request GPIOs are *stall* lines (`periph_busy[i] != 0` makes
  `DMAWFP` wait), so `halo_pdm.c` asserts its `dma-req` output while the FIFO is below the
  watermark — the inverse of what "DMA request" suggests.
- Zephyr encodes `source_data_size = 1` as a **2-byte** burst beat (the PDM driver's
  comment calls it "native 32-bit"), and with `source_addr_adj = NO_CHANGE` it issues two
  beats at the same address per 32-bit scratch word. A 4-byte-only register model returns
  zeros and the FIFO never drains, so the read port pops a set on the first 16-bit beat
  and returns the second half on the next.

Also worth knowing when writing tests: inject a **tone, not DC**. The LPPDM runs with
`iir-bypass`/`fir-bypass`, but `lua_microphone.c` applies its own DC blocker, so a
constant level correctly comes back as zeros.

**3. LC3 was needed, so it is implemented.** Task 3 left this conditional; the condition
is met — `applications/halo/tests/test_speaker_lc3.py` plays a stock
`female_w1_8k_s16.lc3` file, so the decoder has to be interoperable, and `audio_stream.c`
reaches `lc3_api_rom_init` during boot (it was logging "Failed to initialize LC3 codec:
66" and tripping a ROM trap). `rom-stub/src/stub_lc3.c` now backs all ten pinned
`lc3_api_*` symbols with Google's liblc3, pinned at `v1.1.3` and fetched by `init.sh`.

Two things fell out of that:

- liblc3's contexts (up to ~5.3 KB encoder, ~9 KB decoder) do not fit Alif's opaque
  structs (2012 / 1576 / 132 B), but the ABI asks *us* for the sizes of two other
  caller-allocated buffers, so the codec state lives in those. No pool, no allocator, and
  nothing that has to survive `ble_stack_init()` zeroing the ROM data region.
- The whole stub moved to `-mfloat-abi=hard` (hard- and soft-float objects cannot be
  linked together) and the ROM window grew to `0x00090000..0x00190000` to hold liblc3's
  ~115 KB of code and tables above the doorbell rings — which leaves the ring addresses,
  the ABI shared with `hw/arm/halo_ble.c`, untouched.

Verified: a 1 kHz tone injected at the microphone survives encode → decode → speaker at
~990 Hz, and the stock `female_w1_8k_s16.lc3` decodes to recognisable speech.

## Files

- `patches/files/hw/arm/halo_i2s.c`, `halo_pdm.c`, `halo_wav.[ch]` — new models
- `patches/files/hw/arm/halo.c` — PL330 + I2S + PDM wiring, machine options, QOM controls
- `patches/qemu-build-integration.patch` — `select PL330`, new source files
- `rom-stub/src/stub_lc3.c`, `rom-stub/Makefile`, `rom-stub/rom-stub.ld`, `init.sh` — LC3
- `halo-emu` — `--wav-out`, `--wav-in`, `--audio`, `mic`/`speaker` control verbs
- `tests/smoke_audio.py` — the gate
