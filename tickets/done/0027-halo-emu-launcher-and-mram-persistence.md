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

## Implementation notes (2026-08-24, done)

### What was built

- **`halo-emu`** (repo root, python3, no deps): full CLI per the ticket —
  `-f`, `--flash` (default `./mram.img`), `--flash-erase`, `--headless`, `--gdb`,
  `--mcuboot`, `-- <extra qemu args>`. The partition map is documented in the script
  docstring (the repo's single source of truth). Size sanity: firmware ≤ slot0
  `0xC3000`, ≥ `0x1000`; mcuboot ≤ `0x20000`.
- **MRAM persistence** (`qemu/hw/arm/halo.c`): new machine option
  `mram-file=<path>` — MRAM becomes `memory_region_init_ram_from_file(...,
  RAM_SHARED, ...)`, i.e. a MAP_SHARED mmap of the host file. Every guest store
  lands in the page cache immediately, so state survives even a SIGKILL; no
  flush-on-exit path needed. Without the option MRAM stays plain RAM and the old
  `-kernel` flow still works (`armv7m_load_kernel` accepts a NULL filename and
  still registers the mandatory M-profile CPU reset handler).
- **Firmware injection** is the launcher's job, not QEMU's: on every launch the
  `-f` image is written into the slot0 window `[0x20000, 0xE3000)` of the backing
  file with the window's remainder zero-padded (no stale tail from a longer
  previous image); all other regions untouched. `--flash-erase` zeroes the
  non-code regions only: scratch, storage, SE-reserved (slot1 is code, kept).
  When `mram-file=` is used the launcher omits `-kernel` entirely.
- **`--headless`**: accepted; currently a no-op because every run is headless
  until the CDC200 display (0029) — the launcher always passes `-display none`
  today, and the flag keeps invocations stable across 0029.

### mcuboot chain-boot spike (outcome: plumbing works, real mcuboot untested)

- New machine option `svtor=<addr>` (default `0x80020800`) feeds ARMv7M
  `init-svtor`. `--mcuboot <bin>` writes the bootloader at MRAM offset 0 and
  passes `svtor=0x80000000`.
- Validated with a synthetic bootloader (prints `M`, loads the slot0 vector
  table at `0x80020800`, sets MSP, jumps): console shows `M` then the app —
  chain-boot + persistence intact across the jump. A real mcuboot RSA-verify
  boot was **not** exercised: no mcuboot.bin exists on this machine (see gate
  note below). The watchdog stub never fires, so an unconfirmed image cannot
  be rolled back by a modeled watchdog — safe for OTA testing later.

### Gate evidence — synthetic image (real zephyr.bin not available on this host)

`~/halo-firmware` on this machine is a source checkout with **no build
directory** and no `zephyr.bin` anywhere on disk (0026's gate evidence was
produced from a build since removed). The gate's littlefs-log criterion
therefore could not be re-run literally; the underlying mechanics were proven
with a hand-assembled Thumb image (writes magic `0xC0FFEE42` to storage
`0x1A8000`, prints `F` if storage was fresh / `P` if the magic persisted):

- run 1 (blank `mram.img`): `F`; host file contains `42 ee ff c0` at `0x1A8000`
  after SIGTERM (no clean shutdown).
- run 2 (same `mram.img`, slot0 rewritten by the launcher): `P` — storage
  survives a re-flash, exactly the "new firmware, kept filesystem" behavior.
- run 3 (`--flash-erase`): `F` — factory-reset path re-triggers.
- `--mcuboot`: `MF` then `MP` across two runs.
- oversize image (`0xC4000`) rejected; `--gdb` listens on :1234 with the CPU
  halted; bare `qemu -M halo -kernel` (no `mram-file`) unregressed.

**Follow-up when a firmware build exists again**: run
`halo-emu -f .../zephyr.bin` twice and confirm littlefs mounts without
re-format on the second boot (expected to hold — littlefs sees the same
storage bytes the synthetic test proved persistent).
