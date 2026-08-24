# 0020 — Zephyr-BT backend core: manager, connection, advertising parity

**Phase:** 2 — real BLE
**Depends on:** 0019
**Effort:** M/L

## Story

First slice of the Zephyr-host backend: implement `halo/ble_manager.h` +
`halo/ble_connection.h` against `bt_*` APIs so the emulator advertises exactly like a
real Halo and the phone's scan filter matches. The Brilliant app keys on the 128-bit
Lua service UUID in the advertising data.

## Tasks

1. New `modules/halo/src/native_ble/zh_manager.c`:
   `halo_ble_init/adv_start/adv_stop/disconnect/is_connected/get_address/get_conidx`
   (`bt_conn_index`), `get_mtu` = `bt_gatt_get_mtu(conn) - 3` (ATT header), event
   dispatch via the shared registry (0019).
2. Advertising parity (`bt_le_adv_start` with raw `bt_data` arrays) — replicate
   `modules/halo/src/ble_connection.c` exactly:
   - ADV: Flags + Complete Local Name `"Halo XX"` (XX = address byte 3 hex) +
     Complete 128-bit UUID list `7A230001-5475-A6A4-654C-8431F6AD49C4`
   - Scan response: Appearance `0x01C0` (eye-glasses) + 16-bit UUID `0x180F`
   - Intervals 25–125 ms, legacy ADV_IND
   - OMIT LE-Audio announcements and ANCS solicitation (descoped services — don't
     advertise what isn't there)
3. Identity: `CONFIG_BT_PRIVACY=n` (real device uses a stable public address); the
   controller's public address stands in for the EUI-48 → name derivation consistent
   with `frame.get_eui()`.
4. Config fragment `emulator_bt.conf`: `CONFIG_BT=y`, `CONFIG_BT_PERIPHERAL=y`,
   `CONFIG_BT_USERCHAN=y`, `CONFIG_BT_L2CAP_TX_MTU=247`,
   `CONFIG_BT_BUF_ACL_RX_SIZE=251` (+ RX count sized for write-without-response
   floods).
5. Run script `tools/emu/run_bt.sh`: adapter down, `setcap`, `--bt-dev=`, adapter
   restore on exit.

## Key points in code

- `modules/halo/src/ble_connection.c` — advertising payload construction + MTU
  semantics to mirror (`:858` `halo_ble_get_mtu` = bearer MTU − GATT header;
  `frame.bluetooth.max_length()` must equal ATT_MTU − 3 − 1 — off-by-one parity)
- `applications/halo/BLE_SERVICES.md` — advertising/services spec
- Verify: the Brilliant app tolerates missing LE-Audio ADs (they're already crowded out
  on real hardware per BLE_SERVICES.md) — checked properly in 0024

## Acceptance criteria

- [ ] nRF Connect scan record byte-compares to a real Halo (minus descoped ADs)
- [ ] Connect/disconnect fires `HALO_BLE_EVENT_*` through the shared registry
- [ ] MTU 247 negotiated; `frame.bluetooth.max_length()` parity with hardware formula
