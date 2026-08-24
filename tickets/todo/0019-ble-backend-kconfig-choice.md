# 0019 — BLE backend Kconfig choice (ALIF | ZEPHYR_HOST | STUB_TCP)

**Phase:** 2 — real BLE
**Depends on:** 0005, 0018 (go decision)
**Effort:** S

## Story

Three BLE backends now exist conceptually: the Alif ROM stack (hardware), the phase-1
TCP stub, and the incoming Zephyr-host port. Make the selection an explicit Kconfig
choice so builds are self-describing and the source lists stop being ad-hoc.

## Tasks

1. `modules/halo/Kconfig`: replace `HALO_BLE_MANAGER`'s hard
   `depends on ALIF_BLE_HOST` with:
   ```
   choice HALO_BLE_BACKEND
       prompt "Halo BLE backend"
       config HALO_BLE_BACKEND_ALIF        # default on SOC_SERIES_BALLETTO_B1
       config HALO_BLE_BACKEND_ZEPHYR_HOST # depends on ARCH_POSIX && BT_HCI
       config HALO_BLE_BACKEND_STUB_TCP    # phase-1 default on ARCH_POSIX
   endchoice
   ```
2. `modules/halo/CMakeLists.txt`: gate source lists per backend — existing `ble_*.c`
   compile only for ALIF; `emu/transport_tcp.c` + `emu/ble_stubs.c` for STUB_TCP;
   `native_ble/` (0020+) for ZEPHYR_HOST.
3. Factor the **stack-agnostic callback dispatch** out of
   `modules/halo/src/ble_manager.c` (the `halo_ble_callback` list add/remove/fire
   code) into a shared file all three backends link — first shared brick for 0020.
4. `native_sim.conf` selects STUB_TCP; new `applications/halo/configs/emulator_bt.conf`
   fragment (grown by 0020–0023) selects ZEPHYR_HOST.
5. Hardware build (`-b halo`) proves default unchanged.

## Key points in code

- `modules/halo/Kconfig:227` — "Requires: CONFIG_BT=y and CONFIG_BT_CUSTOM=y" (the
  dependency being generalized)
- `modules/halo/src/ble_manager.c` — callback registry to extract (used by pm_manager,
  battery_manager, lua_service)
- `modules/halo/include/halo/ble_manager.h` — the API every backend must satisfy

## Acceptance criteria

- [ ] Three backends selectable; hardware + phase-1 emu builds byte-equivalent behaviour
- [ ] Callback dispatch shared, not duplicated
