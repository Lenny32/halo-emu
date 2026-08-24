# 0012 — Fake IMU sensors (BMA580 accel + QMC6308 magnetometer, tap injection)

**Phase:** 1 — core emulator
**Depends on:** 0009
**Effort:** M

## Story

`lua_imu.c` talks to two devicetree-chosen sensors through the standard sensor API plus
BMA580-private attributes and the on-chip tap engine trigger. Two fake sensor drivers
with settable vectors and a tap-injection hook leave `lua_imu.c` unmodified.

## Tasks

1. `drivers/sensor/emu/emu_bma580.c` — `compatible = "halo,emu-bma580"`, chosen
   `zephyr,accel` in `native_sim.overlay`:
   - standard accel channels backed by a control-plane-settable (x, y, z) vector
   - **accept and record** the private attr_sets from
     `include/drivers/bma580_sensor.h` (ODR, range, tap tuning) — `lua_imu.c` issues
     them at init and via `frame.imu.config/tap_config`
   - tap-injection hook: fire the registered `sensor_trigger` (single/double tap) on
     control-plane `tap 1|2`
2. `drivers/sensor/emu/emu_qmc6308.c` — chosen `zephyr,magn`, settable vector.
   (Heading is currently a firmware stub returning 0.0 — issue #252 — don't build
   fidelity the firmware doesn't have.)
3. Control-plane wiring: `accel x y z`, `magn x y z`, `tap 1|2` (0009).
4. Axis convention: match the **production** mounting remap in `direction()` —
   document the emulator's frame in `EMULATOR.md` (CLAUDE.md gotcha: dev-kit reads ~89°
   off; the emulator should model production, i.e. level = level).
5. `test_imu_raw.py`, `test_imu_direction.py`, `test_taps.py` green with injections.

## Key points in code

- `modules/halo/src/lua_imu.c:234/:241` — chosen-node device grabs; `:255`
  `imu_hardware_init` attr sequence the fakes must tolerate
- `include/drivers/bma580_sensor.h` — private attribute enum (in-repo, compiles anywhere)
- `frame.imu.direction()` (pitch/roll/heading) and `frame.imu.raw()` — Lua surface

## Acceptance criteria

- [ ] `accel 0 0 9.81` → `direction()` reads level; tilts map correctly
- [ ] Injected taps fire `frame.imu.tap_callback` with correct single/double type
- [ ] IMU test subset green

## Note from ticket 0003 (done)

Skeletons already exist in `emulator/module/src/fake_sensors.c`
(`halo,emu-bma580` / `halo,emu-qmc6308`, chosen-wired in the overlay, fixed
level-gravity/zero vectors, BMA580-private attrs swallowed, trigger handlers
recorded but never fired). This ticket extends that file with settable
vectors + tap firing — do **not** create `drivers/sensor/emu/` in the
firmware tree (AGENTS.md). Note the data struct keeps ONE trigger slot per
device; lua_imu registers single- and double-tap separately, so widen it.
