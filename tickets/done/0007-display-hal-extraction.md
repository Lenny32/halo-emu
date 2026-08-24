# 0007 — Display: fake CDC200 scanout buffer (reworked from "HAL extraction")

**Phase:** 1 — core emulator
**Depends on:** 0002 (0005/0006 used for verification)
**Effort:** M
**Status:** DONE — reworked per the emulator hard rule, implemented, 10/10 green subset

## Rework note (why this is not the ticket as originally written)

The original text extracted a 5-function `halo/display_hal.h` by **editing the
firmware tree** (`modules/halo/include/`, `modules/halo/src/display_hal_alif.c`,
`lua_display.c`, `main.c`) and regression-flashing a dev kit. That violates the
hard rule in `emulator/AGENTS.md` (emulator work never touches anything outside
`emulator/`; such a ticket "is wrong: rework it"). It also cannot work
mechanically: the emulator builds the firmware from the **pinned read-only
checkout** in `emulator/src/halo-ws/alif` (see `build.sh`), so firmware-tree
edits never reach the emulator build. The firmware-side HAL extraction remains
a fine upstream refactor, but it is a firmware-repo ticket, not an emulator one.

Equivalent emulator-only seam, per the AGENTS.md injection pattern:

- `lua_display.c`'s entire hardware+canvas path is gated `#ifdef CONFIG_CDC200`.
  The **Kconfig** symbol must stay `n` — setting it (or re-declaring it) makes
  `zephyr/drivers/display/CMakeLists.txt` compile the real Alif driver, whose
  global `cdc200_*` symbols would collide with any fake and poke MMIO. So the
  emulator module injects the **macro only**, at the compiler level:
  `zephyr_compile_definitions(CONFIG_CDC200=1)` — Kconfig and CMake never see
  it, the preprocessor does, and `lua_display.c` compiles its full hardware
  path against the fork's `cdc200.h`/`display_vga020.h` headers (always on the
  include path). Verified: no other source compiled on native_sim references
  the macro.
- The plan's "do NOT fake the cdc200/dsi/vga020 drivers" decision assumed the
  firmware refactor was available; it wasn't. What is faked here is **three
  functions, not drivers**: no devicetree node, no device instance, no driver
  model — the zero-copy property is preserved exactly (canvas rasterizes
  straight into the buffer the fake hands out), and the "explicit present()"
  the plan wanted lives in 0008 as the SDL blit over this buffer.

## What was implemented (all inside `emulator/`)

1. `module/src/display_fake.c` — the three entry points the now-live code calls:
   - `cdc200_get_framebuffer()` → static `uint8_t fb[256][256][3]` (dims from
     chosen `zephyr,display`; the hardware vga020 panel is also 256×256, so
     canvas geometry is conformant). Layer 0 only.
   - `cdc200_set_enable()` → records scanout state (power-save semantics).
   - `vga020_set_pan()` → records the border-register offset (absorbed from
     `misc_stubs.c`; whole-frame shift at presentation, never draw coords).
2. `module/include/halo/emu_display.h` — read-side seam for 0008 (SDL
   presenter) and 0009 (screenshot): `halo_emu_display_fb()`,
   `halo_emu_display_scanout_enabled()`, `halo_emu_display_get_pan()`.
3. `module/Kconfig` — `HALO_EMU_DISPLAY` (default y) gating the macro
   injection; `HALO_BOOT_LOGO` re-declared (drops `depends on CDC200`, keeps
   `select LZ4`) so the boot splash runs on the emulator.
4. `module/CMakeLists.txt` — `display_fake.c` + the gated compile definition.
5. `boards/native_sim.conf` — `CONFIG_HALO_BOOT_LOGO=y` (hardware default hold
   times: boot is ~3 s "late", exactly like a real unit) and
   `CONFIG_SDL_DISPLAY_USE_HARDWARE_ACCELERATOR=n` (the accelerated renderer
   fails under `SDL_VIDEODRIVER=dummy`, which left `sdl_dc` not-ready and took
   the whole display path down headless).
6. `tools/run_emu_tests.py` — green subset extended with `test_display.py`,
   `test_display_bitmap.py`, `test_display_palette.py`, `test_text_api.py`.

Unchanged 0003 wiring that stays by design: chosen `zephyr,panel = &sdl_dc`
(blanking works, is 0008's window-blank seam; PM returns -ENOSYS, tolerated).

## Acceptance (verified 2026-08-24)

- [x] Emulator build green (`emulator/build.sh`, native_sim_64)
- [x] `lua_display.c` compiles its full canvas path unmodified; boot splash
      runs at every boot: scanout on → LZ4 decode + draw → 3 s hold → scanout
      off (log-verified, headless `SDL_VIDEODRIVER=dummy`)
- [x] `run_emu_tests.py` 10/10: M1 core six + all four display tests
- [x] Firmware tree untouched — hardware build/behaviour identical by
      construction (no hardware regression pass needed)

## Consequence for 0008

`halo/display_hal.h` does not exist. The presenter implements against
`halo/emu_display.h` instead, lives in `emulator/module/src/`, and maps:
present = full-frame `display_write()` blit of `halo_emu_display_fb()` to
`sdl_dc` (30 Hz timer while scanout enabled, standing in for continuous
scanout), blank = `display_blanking_on/off` (already wired), pan = offset the
blit by `halo_emu_display_get_pan()`, clamped like the border register.
