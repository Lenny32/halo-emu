# 0010 — Button on gpio_emul (real driver, injected edges)

**Phase:** 1 — core emulator
**Depends on:** 0009
**Effort:** S

## Story

Highest-fidelity fake in the project: reuse the repo's own multi-level button driver
unmodified on top of native_sim's emulated GPIO controller. The single/double/long
(1 s/2 s/5 s/15 s) state machine is real firmware code; only the GPIO line is virtual.

## Tasks

1. `native_sim.overlay`: `button` node, `compatible = "gpio-button"`, on `gpio_emul`,
   alias `sw0` (and `mcuboot-button0` unused), `long-press-ms = <1000 2000 5000 15000>`
   matching `applications/halo/boards/halo.overlay:74-77`.
2. Enable `drivers/input/input_gpio_button.c` for native_sim in the drivers CMake/Kconfig
   (currently built for the halo board only — verify gating).
3. Control-plane handler (`button …` from 0009) → `gpio_emul_input_set` edges with
   sim-time spacing: `click` = press/release < 1 s, `double` = two clicks inside the
   400 ms window, `hold <ms>` = press, wait, release.
4. `test_button.py` green via a wrapper that pairs REPL assertions with control-plane
   injections (test itself unmodified; wrapper script under `tools/emu/`).

## Key points in code

- `drivers/input/input_gpio_button.c` — the driver under test, zero changes
- `include/zephyr/drivers/input/button.h` — `button_callback_register` API used by
  `modules/halo/src/lua_button.c:465` (`DT_ALIAS(sw0)`)
- Double-press window 400 ms; level thresholds from DT — timing injection must use sim
  time consistently (native_sim time ≠ wall clock)

## Acceptance criteria

- [ ] `frame.button.single/double/long` callbacks fire correctly for injected patterns
- [ ] 15 s hold triggers the shutdown path (emu: process exit per 0003)

## Note from ticket 0003 (done)

Tasks 1–2 landed with 0003: `native_sim.overlay` has the `button` node
(`gpio-button` on `gpio0` pin 2, production timings incl. 15 s level) + `sw0`
alias, and the repo's driver builds on native_sim unchanged (it is DT-gated,
not board-gated; `CONFIG_GPIO/INPUT=y` in `native_sim.conf`). Polarity is
**ACTIVE_HIGH** (gpio_emul inputs boot at 0 = released; production is
ACTIVE_LOW) — injection maps press→1/release→0. Remaining: tasks 3–4.
