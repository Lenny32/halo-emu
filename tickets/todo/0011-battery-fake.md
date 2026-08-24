# 0011 — Fake vbat sensor (battery level/charging)

**Phase:** 1 — core emulator
**Depends on:** 0009
**Effort:** S/M

## Story

`battery_manager.c` reads a devicetree `vbat` node through the standard sensor API and
watches charge-state GPIOs. A fake sensor with control-plane-settable voltage leaves the
manager — including its averaging/convergence logic — completely unmodified.

## Tasks

1. `drivers/sensor/emu/emu_vbat.c` — `compatible = "halo,emu-vbat"`, node **labelled
   `vbat`** in `native_sim.overlay` (the label is the contract:
   `battery_manager.c:361` does `DEVICE_DT_GET(DT_NODELABEL(vbat))`, and
   `lua_system.c:416` `DEVICE_DT_GET_OR_NULL` the same). Standard sensor API:
   sample_fetch/channel_get return the settable voltage; accept the attr_sets the
   manager issues.
2. Charging state: settable flag surfaced the same way hardware does (charge-state
   GPIO on `gpio_emul`, or attr on the fake — pick whichever leaves
   `battery_manager.c` untouched; investigate its charge-GPIO usage first).
3. Control-plane wiring: `battery <mv>|<pct>`, `charging on|off` (0009).
4. `test_battery.py` green (note: recent battery filter work — averaging of four ADC
   conversions, convergence toward measured charge — is manager-side logic the fake
   must feed realistically: emit mv, let the manager filter).

## Key points in code

- `modules/halo/src/battery_manager.c:361` — device grab; averaging/convergence in the
  same file (recent commits c2a7b56, 20675d5)
- `drivers/sensor/alif_vbat.c` + `dts/bindings/alif,vbat.yaml` — hardware reference:
  divider ratio, enable GPIO, charge GPIOs
- `frame.battery_level/battery_voltage/battery_charging/charge` — Lua surface to verify

## Acceptance criteria

- [ ] Setting 4200 mV → `frame.battery_level()` converges to ~100; 3500 mV → low
- [ ] `charging on|off` reflected in `frame.battery_charging()`
- [ ] `test_battery.py` green via pyshim + control plane

## Note from ticket 0003 (done)

A skeleton already exists: `emulator/module/src/fake_sensors.c` defines the
`vbat`-labelled node's device (`halo,emu-vbat`, binding in
`emulator/module/dts/bindings/`) with fixed 4000 mV / 80% / not-charging and
records the charge-delta trigger battery_manager registers. This ticket
extends that file (control-plane-settable values, fire the recorded trigger)
— do **not** create `drivers/sensor/emu/` in the firmware tree (AGENTS.md).
Charging is already signalled sensor-side (STDBY_CURRENT != 0), no GPIO.
