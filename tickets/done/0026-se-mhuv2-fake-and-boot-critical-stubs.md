# 0026 — SE/MHUv2 fake + boot-critical peripheral stubs

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0025
**Effort:** L

## Story

Boot the unmodified firmware to its natural pre-BLE limit: banner + logs on the console,
LittleFS mounted on MRAM, `main()` progressing through pm → mem → wdt → led → battery →
file and then **blocking in `alif_ble_enable()`** (that block is expected — ticket 0028
removes it). Everything on that path that is boot-fatal gets a model or stub.

## Tasks

1. **MHUv2 pair + Secure Enclave responder** (the big one; boot panics without it):
   - Devices: sender `0x40050000` (IRQ 38), receiver `0x40040000` (IRQ 37), channel 0 only.
     Register layout per `~/halo-firmware/zephyr/drivers/ipm/ipm_arm_mhuv2.c/.h`:
     channel slots 0x20 bytes (sender `CH_ST@0x00, CH_SET@0x0C, CH_INT_ST@0x10, CH_INT_CLR@0x14,
     CH_INT_EN@0x18`; receiver `CH_ST@0x00, CH_ST_MSK@0x04, CH_CLR@0x08`), block regs at
     +0xF80 (`MHU_CFG, RESP_CFG, ACCESS_REQUEST, ACCESS_READY, INT_ST/CLR/EN`).
   - Handshake: `ACCESS_REQUEST=1` → `ACCESS_READY=1`; on sender `CH_SET` write: clear
     sender `CH_ST` (polled), set `CH_INT_ST` + raise IRQ 38; then deliver the response by
     setting receiver `CH_ST` + IRQ 37; host clears via `CH_CLR=0xFFFFFFFF`.
   - Payload: the 32-bit value written to `CH_SET` is a **global-alias pointer**
     (`0x58...` — translate via the DTCM alias) to a request struct
     (`service_header_t {u16 id; u16 flags; u16 error; u16 pad}` + fields; see
     `~/halo-firmware/modules/hal/alif/se_services/{zephyr/src/se_service.c:73-94,
     include/services_lib_protocol.h, include/services_lib_ids.h}`).
   - Responder semantics: **ack-and-zero** — the driver memsets the struct and treats
     `resp_error_code==0` as success, so writing nothing back already passes
     HEARTBEAT(0), SET_RUN(311), GET_OFF(312), SET_OFF(313), EXTSYS0_BOOT(800),
     UPDATE_STOC(600), BOOT_RESET_SOC(504). Give real payloads only where useful:
     GET_RND(400) → fill the requested bytes from a host RNG (also serves `sys_random_get`),
     GET_TOC_VERSION(200)/GET_DEVICE_REVISION_DATA(208)/FW version(103) → zeros are benign.
   - Boot-order caller list (for testing): `soc_run_profile()` PRE_KERNEL_1 prio 2
     (`~/halo-firmware/zephyr/soc/arm/alif_balletto/b1/soc_b1_dk_rtss_he.c:29`),
     then `halo_pm_init()` → SET_OFF — its failure is the fatal `__ASSERT` in
     `applications/halo/src/main.c:96`.
2. **Watchdog stub**: `arm,cmsdk-watchdog` @ `0x40100000` — accept LOCK(`0x1ACCE551`)/
   LOAD/CTRL/INTCLR writes, never fire (it would fire as NMI; `CONFIG_RUNTIME_NMI=y`).
   App feeds it every 1 s from a coop thread (`modules/halo/src/watchdog_manager.c`).
3. **DW APB RTC** (`snps,dw-apb-rtc`) @ `0x42000000`, 32768 Hz, IRQ 58 — real counter +
   match-interrupt model; it is the `zephyr,cortex-m-idle-timer` (tickless idle breaks
   without it). Driver: `zephyr/drivers/counter/counter_dw_rtc.c`.
4. **VBAT/ADC**: `alif,adc` @ `0x49020000` (+ windows `0x49023000`, `0x1A604000`), IRQs
   151/154/157/160, channel 4 — minimal conversion model returning a configurable battery
   voltage (divider 1k/2.4k; default ≈ healthy 3.9 V equivalent). `-ENODEV` here is a fatal
   assert (`halo_battery_init`, `modules/halo/src/battery_manager.c:361`); reading happens
   at boot then every 10 s.
5. **UTIMER3 PWM sink** @ `0x48004000` (+ global regs `0x48000000`): accept the LED PWM
   programming (`alif,utimer`/`alif,pwm`, driver `pwm_alif_utimer.c`) — `-ENODEV` is fatal
   via `halo_led_init` (`modules/halo/src/led_manager.c:157`). Record duty cycle for later
   (LED state readout, ticket 0031).
6. **DW GPIO** banks gpio0–9 @ `0x49000000..0x49009000` (IRQs 179–253) and LPGPIO @
   `0x42002000` (IRQs 171/172): standard `snps,designware-gpio` model (data/dir/inten
   registers, per-pin IRQ). Needed by regulators, button (later), sen/cam enables.
7. **MRAM as flash**: writes must stick (littlefs `storage` partition @ `0x801A8000`,
   128 KB, erase 1 KB / write 16 B — but since it's plain memory-mapped, RAM semantics are
   already correct). `halo_file_init` mount-fail → auto-format → mount is the expected
   first-boot path; persistent backing file is ticket 0027.
8. **DW I2C0/I2C1 controllers** @ `0x49010000`/`0x49011000` (IRQs 132/133,
   `snps,designware-i2c`): controller state machine with a pluggable target registry; no
   targets required this ticket (sensor/panel targets come in 0029/0031) — absent targets
   must NACK cleanly, not hang.

## Key points in code

- SE wire protocol: `~/halo-firmware/modules/hal/alif/se_services/` (se_service.c:164
  `send_msg_to_se` — pointer-into-mailbox mechanism, `local_to_global()` in
  `common/include/soc_memory_map.h`).
- Fatal-assert map (what this ticket must keep alive): `applications/halo/src/main.c`
  boot order pm → mem → wdt → led → battery → file → ble → splash → lua.

## Gate (acceptance)

- `zephyr.bin` boots to the Halo banner on stdio (`Hardware Version… Firmware Version: 0.8.8-debug`-style), with deferred logging flowing.
- `halo_file` logs show littlefs auto-format + mount success on first boot.
- Execution provably parks in `alif_ble_enable()`'s `k_sem_take(K_FOREVER)`
  (gdbstub thread backtrace) — with no watchdog reset and no SE retry storm.
- SE responder handles the boot sequence: run-profile, (cold boot) SET_OFF, heartbeats.

## Implementation notes (2026-08-24, done)

All models live in `patches/files/hw/arm/` and are wired in `halo.c`
(`halo_create_peripherals()`); `patches/qemu-build-integration.patch` adds them
to the fork's meson/Kconfig (`config HALO` now also selects `DESIGNWARE_I2C`).

- **halo_se.c — MHUv2 pair + SE responder.** One sysbus device, two MMIO
  regions (receiver 0x40040000/IRQ 37, sender 0x40050000/IRQ 38), channel 0,
  `MHU_CFG` reads 1. On sender `CH_SET`: the request struct at the global
  pointer is served in place (structs arrive zeroed, so ack-and-zero is a
  no-op write), sender `CH_ST` stays 0, `CH_INT_ST` latches if `CH_INT_EN`,
  and the receiver's `CH_ST` is raised with the echoed pointer — this
  satisfies both the interrupt path (two IRQs → both driver semaphores) and
  the pre-kernel `poll_out`/`poll_in` path. Real payloads: GET_RND(400) from
  `qemu_guest_getrandom` (serves the BLE BD-address path — note this build
  links **no** entropy driver, so `sys_random_get` isn't a consumer),
  FW-version(103) banner string, GET_TOC_VERSION(200) = 0x01660000. Everything
  else (0/311/313/312/208/504/600/800/…) is acked silently; unknown ids trace
  under `-d unimp`.
- **halo_wdog.c** — CMSDK watchdog register file that never counts down;
  `MASKINTSTAT` reads 0 (`halo_watchdog_has_fired()` false). Deliberate: host
  stalls/gdb must not reboot the guest.
- **halo_rtc.c** — DW APB RTC, CCVR free-running at 32768 Hz off
  QEMU_CLOCK_VIRTUAL **regardless of CCR.EN** and strictly monotonic — the
  driver's CCR shadow is `__noinit` garbage and a CCVR that ever decreases
  makes `cortex_m_systick.c` call the driver's missing `get_top_value` (NULL).
  CMR match sets RSTAT + IRQ 58 when IEN&&!MASK; EOI read clears.
- **halo_adc.c** — conversion on ANY `ADC_CONTROL` bit0 write while
  `START_SRC` bit7 set (the driver re-arms from inside the ISR with RMW-OR,
  so no edge detect); result → `SAMPLE_REG[init_channel]`, `ADC_SEL` =
  channel, DONE1 (bit1, W1C @0x10) → IRQ 154. qdev prop `battery-raw`
  (default 3698 ≙ 3.9 V through the 1k/2.4k divider ⇒ 66% SoC — confirmed in
  the boot log). comp (0x49023000) and AON (0x1A604000) windows are plain
  storage.
- **halo_utimer.c** — utimer3 block as plain register storage (the PWM driver
  never reads hardware status; all RMWs read back) + the 0x24-byte global
  block with RUNNING derived from START/STOP and DRIVER_OEN/CLOCK_ENABLE
  state. LED duty readout for 0031: `COMPARE_A(0xD0) / CNTR_PTR(0xA4)`, gated
  by `COMPARE_CTRL_A(0x8C)` bits 8/9.
- **halo_gpio.c** — DW GPIO port A, 11 instances (gpio0–9 @ 0x4900n000,
  lpgpio @ 0x42002000), per-pin NVIC lines (179+, 171/172), qdev prop
  `ngpios`. `EXT_PORTA = (DR&DDR)|(in&~DDR)` (the driver reads it for outputs
  too), `INTSTATUS = raw & INTEN & ~INTMASK`, EOI W1C. External inputs are
  qdev "in" lines for ticket 0031; they idle at 0 (button released, vbat
  charger-state pin low = not charging).
- **I2C**: QEMU upstream `designware-i2c` (v11.1 has it — reused instead of a
  custom model). Provides the `IC_COMP_TYPE` magic the Zephyr driver checks at
  init and NACK→TX_ABRT→clean -EIO for the absent bma580/pag7982 targets;
  future sensor/panel targets attach as I2CSlaves on its "i2c-bus".
- **Scratch config RAM blocks** in halo.c: EXPSLV 0x4902F000, EXPMST
  0x4903F000, M55HE-CFG 0x43007000. EXPMST is the fix for the display D-PHY
  division-by-zero from the 0025 notes: the driver writes the CDC200 pixclk
  divisor and reads it back; read-as-zero divided by zero.
- **ROM window filler** in halo.c: the BLE/LC3 window (0x90000–0x160000) is
  filled with Thumb `bx lr`. Without it, `ble_task`'s call to the ROM's
  `ble_stack_init` (0x140D55) executed zeros off the window end → fatal
  prefetch abort → `arch_system_halt` before the deferred log ever flushed
  (the silent-console symptom). With it, every ROM entry returns immediately,
  `ble_task` parks in its event loop, and the init callback never fires —
  producing exactly the documented block in `alif_ble_enable()`. Ticket 0028
  replaces the contents with the real synthetic ROM.
- **MRAM/littlefs**: nothing needed beyond 0025's writable MRAM — first boot
  logs `can't mount (LFS -84); formatting` then mounts. Persistence is 0027.

### Gate evidence

- Console (`-serial stdio`): Halo banner, `Hardware Version: halo`,
  `Firmware Version: 0.8.8-debug`, deferred logs for pm → mem → wdt (5000 ms)
  → led → battery → file; littlefs auto-format + mount; battery EMA seeds at
  66 at t+15 s (the 3.9 V default).
- 90 s soak: exactly one banner (no watchdog reset), no SE retries, no error
  storms; `-d unimp,guest_errors` shows only the known NVIC ACTLR RAZ/WI pair
  and four ack-and-zero SE services (311, 313, 208, 800).
- Park proof via gdbstub (`arm-zephyr-eabi-gdb`): CPU in `arch_cpu_idle`;
  `z_main_thread.base.thread_state == 2` (_THREAD_PENDING) and the main-thread
  stack holds return address `0x8002daeb` ∈ `alif_ble_enable`
  (0x8002da7d+0x98) — the `k_sem_take(&rwip_init_sem, K_FOREVER)` call site.
