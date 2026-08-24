# 0027 — `halo-emu` launcher + persistent MRAM

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0026
**Effort:** M

## Story

The user-facing entry point: `halo-emu -f firmware.bin` — take any Halo firmware image
(built locally in `~/halo-firmware` or downloaded) and run it, with the device's 2 MB MRAM
persisted to a host file so `/lfs` (installed `main.lua`, settings, logs) survives restarts
exactly like the hardware's non-volatile MRAM.

## Tasks

1. `halo-emu` wrapper script (repo root):
   - `halo-emu -f <firmware.bin|zephyr.signed.bin> [--flash <mram.img>] [--flash-erase]
     [--headless] [--gdb] [-- <extra qemu args>]`.
   - Accepts both the unsigned `zephyr.bin` and `zephyr.signed.bin` (the imgtool header
     occupies the linked 0x800 pad — same load address `0x80020000`, no special-casing
     beyond a size sanity check ≤ slot0 `0xC3000`).
   - Default MRAM backing file `./mram.img` (2 MB, created blank); `--flash-erase` zeroes
     the non-code regions (factory reset).
2. MRAM persistence in the machine model: back the `0x80000000` 2 MB region with a
   file-mapped block (QEMU `memory-backend-file`/`share=on`, or explicit flush on exit).
   On every launch the `-f` image is written into the slot0 window (`0x20000..0xE3000`)
   of the backing file, leaving `storage` (`0x1A8000..0x1C8000`), slot1, scratch and
   SE-reserved regions untouched — mirroring a device that got a new firmware flashed but
   kept its filesystem.
3. Partition map (single source of truth, document in the script header):
   mcuboot `0x0` +0x20000 | image-0 `0x20000` +0xC3000 | image-1 `0xE3000` +0xC3000 |
   scratch `0x1A6000` +0x2000 | storage `0x1A8000` +0x20000 | SE-reserved `0x1C8000` +0x38000.
4. Optional (time-boxed spike, document outcome): mcuboot chain-boot mode
   `halo-emu --mcuboot mcuboot.bin -f zephyr.signed.bin` — load mcuboot at `0x80000000`,
   vector from `0x80000000`, let it RSA-verify and jump to slot0. Enables OTA/rollback
   testing later; not a gate.

## Key points in code

- Boot facts: entry/vector layout from ticket 0025; `CONFIG_ROM_START_OFFSET=0x800`.
- The firmware confirms its image (`boot_write_img_confirmed()`) only at the very end of
  `main()` — under mcuboot mode an unconfirmed image + modeled watchdog would rollback;
  keep the watchdog stub non-firing.

## Gate (acceptance)

- `halo-emu -f ~/halo-firmware/build/halo/zephyr/zephyr.bin` boots to the ticket-0026
  end-state with one command.
- Two consecutive runs persist littlefs state (boot count / settings file visible in the
  second run's logs, no re-format).
- `--flash-erase` provokes the auto-format path again; `--headless` runs with no display
  dependency (display lands in 0029).
