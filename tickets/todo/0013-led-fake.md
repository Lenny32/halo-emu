# 0013 — Fake LED backend

**Phase:** 1 — core emulator
**Depends on:** 0009
**Effort:** S

## Story

`led_manager.c` drives a single PWM LED via `DEVICE_DT_GET_ONE(pwm_leds)` + the Zephyr
`led` API. Zephyr 3.6 has no PWM emulator, so ship a small fake LED driver whose state
the control plane can query (and optionally paint into the SDL window corner). Until
this ticket, the emulator runs with `CONFIG_HALO_LED_MANAGER=n`.

## Tasks

1. Decide the cheapest faithful cut (investigate first, then pick):
   - (a) fake driver with `compatible = "pwm-leds"` semantics over a stub PWM device, or
   - (b) small `drivers/led/led_emu.c` (`compatible = "halo,emu-led"`) implementing
     `led_set_brightness`, if `led_manager.c`'s `DEVICE_DT_GET_ONE(pwm_leds)` lookup can
     be satisfied/generalized trivially.
   Prefer whichever keeps `modules/halo/src/led_manager.c:157` unmodified.
2. State query via control plane: `led?` → current brightness (0009).
3. Optional: 8×8 px indicator in the SDL window corner on `present()`.
4. Re-enable `CONFIG_HALO_LED_MANAGER=y` in `native_sim.conf`.

## Key points in code

- `modules/halo/src/led_manager.c:157` — the lookup that constrains the design
- `drivers/led_pwm/led.c` — hardware reference
- Boot/charging LED patterns come from the manager — fake only stores brightness

## Acceptance criteria

- [ ] Emulator boots with LED manager on; `led?` reflects manager-driven changes

## Note from ticket 0003 (done)

`emulator/module/src/misc_stubs.c` carries a `halo_led_clear_state()` no-op
under `#ifndef CONFIG_HALO_LED_MANAGER` (lua_button.c calls it
unconditionally). When this ticket re-enables the manager, that stub compiles
away automatically — delete it anyway for hygiene. The fake driver belongs in
`emulator/module/` (AGENTS.md), not `drivers/led/`.
