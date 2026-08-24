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
