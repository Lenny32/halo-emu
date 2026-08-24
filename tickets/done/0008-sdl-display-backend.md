# 0008 — SDL display backend (M2)

**Phase:** 1 — core emulator
**Depends on:** 0005, 0007
**Effort:** M
**Status:** DONE — presenter implemented per the 0007-reworked contract, 10/10 green

> **Contract changed by 0007's rework** (see `done/0007-…md`): there is no
> `halo/display_hal.h` and no `modules/halo/src/emu/` — firmware-tree paths
> below violate the `emulator/AGENTS.md` hard rule. Implement the presenter
> in `emulator/module/src/`, reading `halo/emu_display.h`
> (`halo_emu_display_fb()` 256×256 RGB888, `…_scanout_enabled()`,
> `…_get_pan()`); blanking already reaches `sdl_dc` (chosen `zephyr,panel`).
> The four display tests are already green headless (0007 extended
> `run_emu_tests.py`); this ticket's value is the visible window: RGB888→
> ARGB8888 `display_write()` blits at ~30 Hz while scanout is enabled,
> pan-offset applied at blit time. `SDL_VIDEODRIVER=dummy` + the already-set
> `CONFIG_SDL_DISPLAY_USE_HARDWARE_ACCELERATOR=n` cover headless/CI.

## What was implemented (all inside `emulator/`)

1. `module/src/display_present.c` — a ~30 Hz presenter thread
   (`K_THREAD_DEFINE`, lowest application priority; rate =
   `CONFIG_HALO_EMU_DISPLAY_FPS`, new module Kconfig, default 30) standing
   in for continuous hardware scanout:
   - While `halo_emu_display_scanout_enabled()`: convert the canvas RGB888
     buffer to ARGB8888 (SDL's packed-32-bit default format — endian-safe
     `0xAARRGGBB` words) and `display_write()` the full frame to chosen
     `zephyr,panel` (`sdl_dc`). Scripts that draw without `show()` update
     the window anyway, like on hardware.
   - Pan applied at blit time as a whole-frame shift, black shifting in at
     the borders. Sign convention mirrors `display_vga020.c` (+x right,
     +y down); an off→on scanout edge forces a re-present of an unchanged
     frame (blank/unblank of a static screen).
   - Idle-frame skip: an RGB888 shadow copy of the last-presented frame
     gates the SDL blit (memcmp), so a static screen costs nothing.
   - No blanking code: `lua_display.c` itself drives
     `display_blanking_on/off` on the panel (SDL driver implements both);
     the presenter only stops blitting while scanout is off. Torn frames on
     a mid-draw blit are accepted — hardware scanout has the same property.
   - No explicit hook in `frame.display.show()`: that would need a
     firmware-tree edit (hard rule); the 30 Hz cadence covers it — worst
     case one frame period of latency, invisible in practice.
2. `module/src/display_fake.c` — `vga020_set_pan()` now clamps to
   [-50, 50] exactly like the real border-register driver, so the presenter
   and the 0009 screenshot seam see the effective offset, not the request.
3. `module/CMakeLists.txt` — `display_present.c` compiled under
   `CONFIG_HALO_EMU_DISPLAY` + `CONFIG_SDL_DISPLAY`;
   `module/Kconfig` — `HALO_EMU_DISPLAY_FPS` (int, 1–120, default 30).
4. `tools/run_emu_tests.py` — launches the emulator with
   `SDL_VIDEODRIVER=dummy` by default (override by exporting it, e.g.
   `SDL_VIDEODRIVER=x11` to watch tests draw): since this ticket a real
   window would otherwise pop up during every test run. Headless mode
   documented in `EMULATOR.md` (CI wiring is ticket 0017).

Brightness needs no backend work: `frame.display.set_brightness/brightness`
persist a settings value and call `display_set_brightness()` on the panel,
which the SDL driver doesn't implement — same tolerated -ENOSYS path as PM,
already exercised by the resume handler since 0007.

## Key points in code

- `module/include/halo/emu_display.h` — read seam from 0007 (fb 256×256
  RGB888 physical orientation, scanout state, pan)
- `native_sim.overlay` chosen `zephyr,display = zephyr,panel = &sdl_dc`,
  256×256 (canvas dimensions resolve from this node)
- Palette/assign_color path is canvas-side — no backend work
- Pan is a whole-framebuffer offset (hardware border register), never
  per-draw-call; clamp [-50, 50] per `display_vga020.c:624`

## Acceptance criteria (verified 2026-08-24)

- [x] Emulator boots → logo visible in SDL window (WSLg): presenter thread
      blits during the 3 s splash hold (scanout on 0→3.01 s, no
      display_write errors) → `frame.display.*` drawing works over the REPL
- [x] M2 test subset green through the pyshim: 10/10
      (`run_emu_tests.py` — M1 six + all four display tests)
- [x] `power_save`/resume cycle blanks/unblanks the window: REPL smoke
      `power_save(false)` → scanout on + `display_blanking_off`,
      `power_save(true)` → blanking on + scanout off; presenter gates on
      the scanout state (re-presents on the off→on edge)
