# 0031 — Inputs & runtime controls (button, battery, reboot)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0029, 0030
**Effort:** M

## Story

Drive the emulated device the way a user/tester drives hardware: press the button, change
the battery level, observe the LED — scripted (control socket/QMP) and interactive (key
binding in the QEMU window).

## Tasks

1. **Button**: the single hardware button is `gpio-button` on **LPGPIO pin 1, active-low**
   (`lpgpio@42002000`, IRQs 171/172; driver `alif/drivers/input/input_gpio_button.c`,
   own thread, 30 ms debounce, long-press 1/2/5/15 s, double-click 400 ms).
   Add press/release control: QMP command or control-socket verb `button down|up|click|
   hold <ms>`; bind a key (e.g. `B`) in the QEMU window for interactive use. The GPIO
   model (0026) must deliver edge IRQs for both directions.
2. **Battery**: runtime-settable voltage/level on the 0026 ADC model (`battery set 82%`),
   plus charger-state GPIOs (`state-gpios` gpio1.3, `charge-control` gpio0.6) so charge/
   discharge UI states are testable. Firmware samples every 10 s — document the latency.
3. **LED readout**: expose the UTIMER3 PWM duty (0026 records it) via the control socket
   (`led?` → duty/period) for test assertions.
4. **Lifecycle hooks**: `reboot` (SE BOOT_RESET_SOC(504) → machine reset, MRAM backing
   file preserved), `wdt-fire` (make the 0026 watchdog stub actually expire once: NMI →
   `watchdog_fired` magic + reset — tests `halo_watchdog_has_fired()` cold-boot path).
5. Wire the control socket into `tools/run_emu_tests.py` for the device tests that need
   button/battery interaction.

## Gate (acceptance)

- Scripted click / 2 s hold reach the firmware's Lua button handlers (observable via REPL).
- Battery set → reflected in `frame.battery` readings within one 10 s poll.
- `wdt-fire` → reboot → firmware logs the watchdog-fired cold boot path.

## Implementation notes (done 2026-08-24)

Gate results (all via `tests/smoke_controls.py`, green against the 0.8.8
release binary): scripted `button click` fires `frame.button.single`,
`button hold 1200` fires `frame.button.long`; `battery set 82%` shows up
exactly (4048 mV) in `frame.battery_voltage()` within one 10 s poll;
`wdt-fire` reboots through the watchdog-fired cold-boot path (verified as
two QMP RESET events 5 ms apart: the model's warm reset, then the
firmware's magic-detected `sys_reboot(COLD)` via the SE) with `/lfs`
intact.  Ticket-text corrections from the firmware source (read-only
reference, `alif/` in the halo-ws workspace):

- The **Lua** button handlers are `frame.button.single/double/long`
  (`lua_button.c`); `long` is the **1 s** level and fires on release.  The
  2/5/15 s levels are hard-wired in C (deep sleep / pairing window / ship
  mode — not Lua), so the gate exercises click + 1.2 s hold.  Button
  events arm a Lua debug hook: the callback runs on the next VM activity,
  so tests pump the VM with a `print()` after injecting.  The gate's
  "2 s hold" wording predated this; deep sleep is deliberately not
  triggered by the smoke test.
- There is no `frame.battery` table: the flat `frame.battery_voltage()`
  (unfiltered, poll-fresh) / `frame.battery_level()` (EMA-filtered, lags
  by design) / `frame.battery_charging()`.  The gate asserts on voltage.
- The firmware never logs the watchdog cold-boot detection (`main.c`
  checks `halo_watchdog_has_fired()` silently before the banner); the
  observable is the double reboot, which the smoke test (client drop +
  REPL recovery + `/lfs`) and the RESET-event count cover.

Mechanics:

- Control socket: `halo-emu --ctl-port` (default 9562), a line-oriented
  text protocol (`button down|up|click|hold`, `battery set/?`,
  `charger on|off|?`, `led?`, `reboot`, `wdt-fire`, `ping`) implemented
  in the launcher (`CtlServer`) over a private QMP monitor —
  everything maps to `qom-get`/`qom-set` on stable `/machine` properties
  (`button-pressed`, `charger-connected`, `charge-enabled`,
  `battery-raw`, `led-duty/led-period/led-on`, `wdt-fire`), plus QMP
  `system_reset` for `reboot`.  No QAPI schema changes.  The `B` key in
  the QEMU window presses/releases the button (keyboard input handler in
  `halo.c`; the machine has no other keyboard).
- Button: drives lpgpio "in" pin 1 (active-low).  The driver only needs
  the first EDGE_BOTH interrupt — its thread then polls the level every
  20 ms (`input_gpio_button.c`), which the 0030-era GPIO model already
  satisfies.  GPIO external input levels now survive machine resets
  (`ext_in` seeded once from `in-default`): a held button stays held
  across a reboot, and the reset-ordering trap (legacy reset handlers run
  *before* device resets) is avoided entirely.
- Battery: ADC `battery-raw` became a runtime QOM property; the launcher
  converts `82%` through the firmware's own 21-point discharge curve
  (`alif_vbat.c`) and mV→raw via the 2.4k/1k divider (raw = mV·4095/4320).
- Charger: STAT gpio1.3 is *physically high* while charging (the vbat
  driver double-negates its ACTIVE_LOW flag); the driver re-samples on
  the STAT edge, so `frame.battery_charging()` follows with no poll wait.
  `charger?` also decodes the firmware's gpio0.6 charge-control output
  (tri-state input = enabled, output-low = cut) from new `dr`/`ddr`
  readout props on the GPIO model.
- LED: UTIMER3 `COMPARE_A`/`CNTR_PTR`/`COMPARE_CTRL_A.DRIVER_EN` exposed
  as `led-duty`/`led-period`/`led-on`.
- wdt-fire: the watchdog model latches RAWINTSTAT and raises a new NMI
  line (wired to the armv7m "NMI" input); Zephyr's `wdt_cmsdk_apb_isr`
  reads MASKINTSTAT ≠ 0 and runs the app callback, which stores the
  `0x57444649` `__noinit` magic.  200 ms (virtual) later the model
  performs the RESEN second-timeout reset — *TCM-preserving*
  (`halo_sram_preserve_next_reset()`, new `halo.h` hook): a watchdog
  reset is warm, unlike the SE reboot's TCM power-cycle, and the magic
  must survive exactly one reset.  The injected expiry resets
  unconditionally (a 1 Hz feed cannot cancel it) so tests stay
  deterministic.
- Test wiring: `tools/run_emu_tests.py` launches with `--ctl-port` and
  exports `HALO_EMU_CTL`; `pyshim/brilliant_ble.emu_control()` is the
  emulator-only helper for device tests that need button/battery
  interaction (the suite's `test_button.py` remains human-interactive
  and is not in the M1 subset).
- Regressions re-run green: `tools/repl_smoke.py` (0.8.8 release binary)
  and the M1 device-test subset (locally built sysbuild image).
