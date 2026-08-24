# 0023 — OTA over real BLE via Zephyr mcumgr SMP transport

**Phase:** 2 — real BLE
**Depends on:** 0020
**Effort:** S/M

## Story

Free win: the firmware's hand-rolled SMP GATT service (`ble_ota.c`) deliberately mirrors
upstream Zephyr mcumgr's SMP-over-BT UUIDs (`8D53DC1D-…` / `DA2E7828-…`). On the
Zephyr-host backend, enable the real mcumgr subsystem instead of porting `ble_ota.c` —
`ota_flash.py` should work up to and including image upload into simulated flash.

## Tasks

1. `emulator_bt.conf`: `CONFIG_MCUMGR=y`, `CONFIG_MCUMGR_TRANSPORT_BT=y` (+
   `_AUTHEN` so encryption gating matches the hardware's `SEC_LVL(WP, UNAUTH)`),
   `os_mgmt`, `img_mgmt`.
2. Flash: image-0/image-1 partitions on flash_sim in `native_sim.overlay` sized like
   hardware (780 K each) so `img_mgmt` uploads land somewhere real.
3. native_sim `img_mgmt` shim where needed: "test/confirm + swap and boot new image"
   is meaningless without MCUboot on posix — return success on confirm OR document the
   limit in `EMULATOR.md` (upload + hash verify is the meaningful part).
4. Exclude Alif SE update path (`modules/halo/src/se_mgmt.c`, SMP group 64) — hardware
   only.
5. Verify with `applications/halo/tools/ota_flash.py` against the emulator.

## Key points in code

- `modules/halo/src/ble_ota.c:44` — hand-rolled `struct smp_transport` registration
  (stays ALIF-backend-only after this ticket)
- `applications/halo/tools/ota_flash.py`, `tools/verify.py` — the clients to satisfy
- `boards/arm/halo/halo.dts` fixed-partitions — sizes to mirror

## Acceptance criteria

- [ ] `ota_flash.py` uploads a signed image into sim flash over real BLE, hash verifies
- [ ] Behaviour on confirm documented (success-stub or explicit unsupported)
