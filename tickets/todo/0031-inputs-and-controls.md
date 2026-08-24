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
