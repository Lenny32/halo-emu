# 0022 — Zephyr-BT battery service + pairing/bonding policy

**Phase:** 2 — real BLE
**Depends on:** 0020
**Effort:** M

## Story

Two remaining behaviour pillars for "the phone sees a real Halo": the battery service
(custom — Zephyr's BAS lacks the Power State char the firmware exposes) and the pairing
policy from `PAIRING.md` (LESC Just Works, 5-slot LRU bond table, 60 s pairing window).
Zephyr's host does the crypto/keys; only the policy is reimplemented.

## Tasks

1. `modules/halo/src/native_ble/zh_battery.c` — service `0x180F` with Level `0x2A19`
   (Read/Notify) and Power State `0x2A1A` (Read/Notify), fed by the emulated battery
   manager events (0011) through the shared callback registry.
2. `modules/halo/src/native_ble/zh_security.c`:
   - auth callbacks NoInputNoOutput (LESC Just Works)
   - pairing window: 60 s work item; connect-time policy per PAIRING.md §3.2 —
     accept known bonds anytime, accept unknown only inside the window, reject
     otherwise; `bt_unpair` on encryption-failure isolation
   - bond storage on the phase-1 settings/littlefs backend
3. Config: `CONFIG_BT_SMP=y`, `CONFIG_BT_BONDABLE=y`, `CONFIG_BT_MAX_PAIRED=5`,
   `CONFIG_BT_KEYS_OVERWRITE_OLDEST=y` (closest upstream analog to the LRU policy),
   `CONFIG_BT_SETTINGS=y`.
4. Known gap to verify/document: Zephyr orders keys by storage age, not last-use —
   exact LRU-bump-on-reconnect parity is a stretch goal, note the difference in
   `EMULATOR.md`. Also verify re-pair after phone "Forget Device"
   (`CONFIG_BT_SMP_ALLOW_UNAUTH_OVERWRITE` — confirm option name in the 3.6 fork).

## Key points in code

- `modules/halo/src/ble_security.c` (1122 lines) + `PAIRING.md` — the policy spec
- `modules/halo/src/ble_battery.c` — char layout/notify triggers to mirror
- Bonds persist across emulator restarts via `--flash=halo_flash.bin` (0004)

## Acceptance criteria

- [ ] Phone pairs Just Works inside window; rejected outside; bonded phone reconnects
      encrypted anytime
- [ ] 6th bond evicts the oldest; bonds survive emulator restart
- [ ] Battery level/state readable + notifying on control-plane changes
