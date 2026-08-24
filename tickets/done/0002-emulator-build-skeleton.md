# 0002 — Emulator build skeleton: out-of-tree module, fragments, build script

**Phase:** 1 — core emulator
**Depends on:** 0001
**Effort:** M

## Story

Decision (revised): **one app, zero firmware-tree changes**. `applications/halo`
builds for `native_sim` **unmodified** — `main.c`'s boot order (pm → mem → wdt →
led → battery → file → ble → splash → lua_runtime) is part of what we emulate.

**Hard rule for this and every emulator ticket: nothing outside `emulator/` may
be created or modified.** Everything emulator-specific is injected at build
time:

- an **out-of-tree Zephyr module** (`emulator/module/`, passed via
  `-DZEPHYR_EXTRA_MODULES`) carries all emulator Kconfig symbols and (from
  0003 on) all emulator C code — stub headers/implementations for the
  hardware-only APIs, the TCP transport, the fakes;
- config/overlay fragments in `emulator/boards/` are passed per-build. The
  app's CMakeLists force-caches `EXTRA_CONF_FILE=debug.conf`, so the config
  fragment rides `-DCONF_FILE="<app>/prj.conf;<fragment>"` instead (debug.conf
  still appends); the overlay uses `-DEXTRA_DTC_OVERLAY_FILE`;
- `emulator/build.sh` wraps the invocation.

## Tasks

1. `emulator/module/` Zephyr module (`zephyr/module.yml`, `Kconfig`,
   `CMakeLists.txt`):
   - `HALO_EMULATOR` (bool, `depends on ARCH_POSIX`, default y; `select
     HAS_PM` — HALO_PM_MANAGER's unconditional `select PM` is otherwise a
     fatal y-select with unmet deps on POSIX; the missing `pm_state_set()`
     hooks become link errors that 0003 stubs in this module)
   - `HALO_TRANSPORT_TCP` (default y), `HALO_EMU_TCP_PORT` (9563),
     `HALO_EMU_CTRL_PORT` (9564), `HALO_EMU_MTU` (512)
   - **re-declarations** of `HALO_BLE_LUA_SERVICE` and its three buffer ints
     (Kconfig ORs dependencies across definition locations): keeps the Lua
     runtime's transport dependency satisfiable without the Alif BLE manager.
     modules/halo compiles ble_lua.c only under `HALO_BLE_MANAGER`, so the
     module provides the `halo/ble_lua.h` API over TCP instead (0005).
2. `emulator/boards/native_sim.conf`: `HALO_EMULATOR=y`, BLE manager +
   OTA/ANCS/audio services off, watchdog off, AEC off (returns in 0015),
   sounds/boot-logo off, no external SRAM + big `HALO_MEM_INTERNAL_SIZE`,
   `LOG_MODE_DEFERRED=y`, `SDL_DISPLAY=y`, `FLASH_SIMULATOR=y`.
3. `emulator/boards/native_sim.overlay`: chosen `zephyr,display = &sdl_dc`
   at 256x256 (canvas resolves PHYS_WIDTH/HEIGHT from the chosen display);
   the board dts already has the flash simulator with a `storage_partition`
   label matching `STORAGE_PARTITION` in `modules/halo/src/file_manager.c`.
   Placeholder comments for the fake nodes of tickets 0010-0016.
4. `emulator/build.sh`: venv + `ZEPHYR_TOOLCHAIN_VARIANT=host` + the
   `-DZEPHYR_EXTRA_MODULES/-DCONF_FILE/-DEXTRA_DTC_OVERLAY_FILE` invocation;
   `BOARD=native_sim` env override for the 32-bit variant. Both native_sim
   flavours share the same fragments.
5. `emulator/EMULATOR.md` (build/run doc, grows with each ticket).

## Key points in code

- `emulator/build.sh` — the only supported way to build the emulator
- `applications/halo/prj.conf` — read-only base the fragment overrides.
  ~21 benign Kconfig warnings remain ("X was assigned but got ''"): prj.conf
  values for hardware-gated ints/strings (AEC taps, SRAM window, watchdog,
  sound cues, ...) whose menus are off on native_sim. These are non-fatal;
  fatal kconfiglib warnings (selects with unmet deps) are all resolved.
- Build won't link yet (missing stubs/transport) — tickets 0003-0005. This
  ticket's target is configure + Kconfig resolving for native_sim.

## Acceptance criteria

- [x] `emulator/build.sh` reaches the compile stage (configure + Kconfig
      resolve; `HALO_EMULATOR/HALO_TRANSPORT_TCP/HALO_LUA_RUNTIME=y` in
      .config). Compile failures mapped: `se_service.h`/`power_mgr.h`/
      `gap_le.h`/`t5838.h` → 0003/0005, `canvas.h` → 0007, DT `vbat`/
      `zephyr,accel`/`zephyr,magn`/LED nodes → 0011/0012/0013.
- [x] `git status` shows **no change outside `emulator/`** — hardware build
      untouched by construction.
