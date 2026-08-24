# 0004 — Filesystem: littlefs on flash_sim, persistent /lfs

**Phase:** 1 — core emulator
**Depends on:** 0002
**Effort:** S
**Status: DONE** (2026-08-24)

## Story

`/lfs` is the device's installed-app storage (`main.lua`, settings, logs). On native_sim,
Zephyr's flash simulator backs the same littlefs mount; a host file makes it persist
across emulator runs, mirroring MRAM.

## What was done

Config-only ticket: two fragment edits + docs, zero C code, firmware tree untouched.

- **`emulator/boards/native_sim.overlay`** — reshaped the simulated flash to mirror
  hardware MRAM:
  - `&flash0 { erase-block-size = <1024>; write-block-size = <16>; }` — the Balletto
    `mram_storage` geometry (`zephyr/dts/arm/alif/balletto_fpga_rtss_common.dtsi:324`),
    so littlefs computes the identical filesystem layout as on device (native_sim's
    stock 4096/1 would also mount, but images would not be geometry-interchangeable).
  - `&storage_partition { reg = <0x000fc000 DT_SIZE_K(128)>; }` — grown in place from
    native_sim's stock 16 KB to the hardware size (`halo.dts:305`); nothing sits above
    0xfc000 and 0xfc000+128 KB ends well inside the 2 MB `flash0`.
- **`emulator/boards/native_sim.conf`** — `CONFIG_FLASH_SIMULATOR=y` was already there
  from 0002; comment now documents where the rest of the stack comes from
  (`FLASH/FLASH_MAP/FLASH_PAGE_LAYOUT` from prj.conf; `FILE_SYSTEM(_LITTLEFS)` are
  **selects** in `modules/halo/Kconfig:87` so no board defconfig is needed) and the
  persistence flags.
- **`emulator/EMULATOR.md`** — new "Filesystem" section: geometry rationale,
  persistence flags, seeding story; status + runtime-gaps updated.

## Verification (task 1 + acceptance)

- `file_manager.c` mounts unmodified by inspection: `FIXED_PARTITION(storage_partition)`
  resolves against native_sim's node label (same label as halo.dts); mount at
  `file_manager.c:101`, erase+remount fallback at `:106`; `lua_file.c:620` auto-creates
  an empty `main.lua`, so a blank flash image boots clean.
- `emulator/build.sh`: all objects compile, generated `zephyr.dts` shows
  `erase-block-size 0x400 / write-block-size 0x10 / storage 0xfc000+0x20000`; link
  failure is still exactly ticket 0005's ten `halo_ble_*` symbols; warning set
  unchanged (the ~21 known benign Kconfig ones).
- **Geometry + persistence proven at runtime** (zephyr.exe can't link until 0005, so —
  as the ticket suggests — via `zephyr/samples/subsys/fs/littlefs` built for
  native_sim_64 with the exact same two overrides):
  `FS at flash-controller@0:0xfc000 is 128 0x400-byte blocks`, boot_count incremented
  1 → 2 → 3 across three runs of `./zephyr.exe --flash=halo_flash.bin` (2 MB backing
  file). No geometry warnings — littlefs defaults hold: prog/read 16 | cache 64 |
  block 1024 | partition. The only first-run message is littlefs's expected
  "Corrupted dir pair" on pristine 0xFF flash, which is what triggers the auto-format
  path (in the app: file_manager's erase+remount).

## Persistence semantics (documented in EMULATOR.md)

The flash simulator is **file-backed by default**: it mmaps `./flash.bin` (created
blank on first run) in the launch directory. `--flash=halo_flash.bin` names the file,
`--flash_erase` factory-resets at boot, `--flash_in_ram` makes the run throwaway.
`/lfs/settings` (`CONFIG_SETTINGS_FILE_PATH`) rides along: brightness, pan, volume
persist across emulator restarts like on device.

## Decisions

- **mklfs.py seeding helper (task 4): skipped** — explicitly optional in the ticket;
  the 0005 file API over the socket seeds `main.lua`, and an empty image boots clean
  anyway. If ever needed: littlefs-python, block_size=1024, image at offset 0xfc000.

## Key points in code

- `emulator/boards/native_sim.overlay` — `&flash0` geometry + `&storage_partition` size
- `modules/halo/src/file_manager.c:33` — mount point `/lfs`, partition label (untouched)
- `modules/halo/src/lua_file.c:620` — empty `main.lua` auto-created if missing
- `modules/halo/Kconfig:87` — `select FILE_SYSTEM`/`FILE_SYSTEM_LITTLEFS` (why no
  extra fs symbols are needed in the emulator fragment)

## Acceptance criteria

- [x] Emulator mounts `/lfs` and files survive a restart with `--flash=halo_flash.bin`
      — proven with the littlefs sample at identical geometry (boot_count 1→2→3);
      full-app mount re-checked once 0005 links (`main.lua` creation is code-inspected)
- [x] littlefs geometry warnings absent from boot log
