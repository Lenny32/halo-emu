# 0037 — I2C0 motion sensors: BMA580 IMU + QMC6308 magnetometer

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0035 (I2C reads work at all), 0031 (control-socket pattern)
**Effort:** M

## Story

`halo.c` attached I2C slaves only to I2C1 (vga020, TPS65132). The devicetree also has
`bma580@18` and `qmc6308@2c` on **I2C0** (`zephyr.dts:400-419`), both `status = "okay"`
with `lazy-init` — so the boot log stays clean and they fail only when something touches
them. Lua does: `lua_imu.c` exposes `frame.imu.raw()` (accel + compass),
`frame.imu.direction()` (pitch/roll/heading) and `frame.imu.tap_callback()`. Before 0035
this was moot — no attached target could be read at all — so modelling them is only now
worthwhile.

**`halo_i2c_regfile` is not sufficient.** Both drivers gate on a chip-ID read, and a
register file returns 0 for anything unwritten:
- BMA580: `CHIP_ID` (0x00) must read `0xC4` (`bma580.h:56,62`), checked at
  `bma580_features.c:108`. Worse, `HEALTH_STATUS` (0x02) low nibble must read `0x0F`:
  `bma580_init()` polls it in an **unbounded `for(;;)`** (`bma580_features.c:122-137`), so
  a wrong value hangs the guest instead of failing it.
- QMC6308: `CHIPID` (0x00) must read `0x80` (`qmc6308.c:20,34,442-449`), and `STATUS`
  (0x09) bit0 (DRDY) must set or `sample_fetch` returns `-ETIMEDOUT`
  (`qmc6308.c:271-292`).

## Tasks

1. `patches/files/hw/arm/halo_bma580.c` and `halo_qmc6308.c` — register files with the
   correct reset values, sample registers served from injected state (little-endian int16
   per axis, the layout the drivers reassemble at `bma580_driver.c:239-241` and
   `qmc6308.c:300-302`), and for the IMU a write-1-to-clear `INT_STATUS_INT1_0/1`
   (0x12/0x13) plus an INT1 output line.
2. Attach both to the I2C0 bus in `halo.c`; wire IMU INT1 to **gpio3.2**
   (`int1-gpios = <&gpio3 2 0>`) through the existing `halo-dwgpio` named "in" lines.
   Register the models in `patches/qemu-build-integration.patch` (`hw/arm/meson.build`) —
   unlike an overlay of an existing upstream file, a *new* model must be added there.
3. Machine properties `accel-x/y/z`, `magn-x/y/z`, `temp`, `tap`, `data-ready`, forwarded
   to the devices like the 0031 controls; control-socket verbs `accel`, `magn`, `tap`.
   The machine stays in raw LSB counts (as `battery-raw` does); the launcher does the
   milli-g / milli-gauss arithmetic.
4. `tests/smoke_sensors.py` — inject over the control socket, read back through the Lua
   REPL, assert the orientation math and that tap triggers reach the Lua callback.

## Gate

- `./halo-emu -f 0.8.8.bin` boots with no new I2C errors; `frame.imu.raw()` returns the
  injected sample instead of failing; taps reach `frame.imu.tap_callback`.
- `tests/smoke_sensors.py` green; the other four smoke tests still pass.

## Outcome (done 2026-08-26)

1. Both models added and wired as above. Scales verified against the firmware rather than
   assumed: the devicetree's `range = <2>` is **2 g**, because the driver reads it with
   `DT_INST_ENUM_IDX` (`bma580_driver.c:934`) and the binding's enum starts at 2 — so the
   scale is 16384 LSB/g, not the 4096 of the 8 g case this ticket first assumed. The
   round-trip test caught it (`accel 0 0 1000` read back as 250 mg).
2. **A second upstream controller bug fell out of this work** — see `EMULATOR.md` and the
   overlay `patches/files/hw/i2c/designware_i2c.c`. `dw_ic_data_cmd_reg_post_write` ran its
   repeated-START block *after* the send/receive, but the guest tags the **first** byte of
   a message with RESTART. So on a two-byte register write `[reg, value]` the controller
   issued a START between the two bytes and the target latched `value` as a new register
   address — every register write to an I2C target was silently landing in the wrong place.
   Reads were unaffected (their restart already preceded the receive), which is why 0035
   fixed reads and this stayed hidden: nothing on the display path ever verified a written
   register. Fix: handle RESTART before the address/data phase. Found by tracing why the
   IMU still failed with `-5` after the models were correct.
3. **Gate evidence:**
   - `tests/smoke_sensors.py`: 12/12 green. Injected accel `1000,-500,250` mg reads back
     exactly through `frame.imu.raw()`; magnetometer `200,-300,400` mgauss likewise;
     `accel 1000 0 0` reads worn-level and `accel 0 0 1000` reads roll -90, matching the
     firmware's device→host remap (up = dev.X, `lua_imu.c:370-387`); single and double taps
     reach the Lua callback over INT1.
   - `direction().heading` is asserted to be **0.0 by design** — the firmware hard-codes it
     (`lua_imu.c:392-399`: tilt-compensated heading needs per-unit hard-iron calibration
     the firmware does not have), so a compass-angle assertion would be wrong.
   - Full suite after the restart-ordering change: `smoke_display`, `smoke_ble`,
     `smoke_controls`, `smoke_audio`, `smoke_sensors` all pass, 0 orphaned processes.

## Follow-up

The PAG7982 camera @`0x40` on I2C1 is still unattached and its DT node is **not**
lazy-init — that stays with ticket 0033.
