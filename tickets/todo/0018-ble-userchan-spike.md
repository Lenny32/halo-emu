# 0018 — Phase-2 spike: Zephyr BT host over HCI userchan (GATES phase 2)

**Phase:** 2 — real BLE
**Depends on:** 0001 (workspace only; independent of phase-1 tickets)
**Effort:** S (0.5–1 day, time-boxed)

## Story

Phase 2 makes the emulator advertise as a real Halo over the host machine's Bluetooth
adapter (Zephyr BT host inside native_sim, HCI userchan to a real controller). The
firmware's own BLE stack (Alif ROM, `CONFIG_BT_CUSTOM`) can never run off-silicon —
so everything hangs on the Zephyr fork still having a working `subsys/bluetooth` host
and `drivers/bluetooth/hci/userchan.c`. Prove it in one afternoon before committing to
the port. Failure fallback (documented, not built): external BlueZ GATT bridge.

## Tasks

1. Build the fork's upstream sample for native_sim:
   `west build -b native_sim zephyr/samples/bluetooth/peripheral -- -DCONFIG_BT_USERCHAN=y`
2. Run against a dedicated adapter (USB dongle strongly advised — Realtek RTL8761B or
   Intel internal known-good; avoid CSR clones). **These commands change host BT state
   (adapter down, sudo/setcap) — list them and get user approval before running;
   restore adapter state afterwards:**
   ```
   sudo btmgmt -i hci0 power off       # adapter must be DOWN, BlueZ must not re-grab
   sudo ./build/zephyr/zephyr.exe --bt-dev=hci0    # or setcap cap_net_admin+ep
   ```
3. From nRF Connect (phone): scan → connect → MTU 247 → subscribe to a notify char →
   LESC Just Works pair → disconnect → reconnect encrypted.
4. Verify in the fork (mark each): `drivers/bluetooth/hci/userchan.c` present;
   `CONFIG_BT_HCI` selectable alongside Alif's `BT_CUSTOM` (Alif must not
   `select BT_CUSTOM` unconditionally under `BT`); `--bt-dev=127.0.0.1:PORT` proxy
   syntax available.
5. Record results + adapter model in `EMULATOR.md`. If structurally broken and not
   patchable in the fork (it's Brilliant's own fork — Kconfig fixes are in scope),
   write the BlueZ-bridge fallback decision here and re-plan.

## Key points in code

- `west.yml` zephyr pin `190f4cb8` — the tree under test
- `applications/halo/boards/halo.conf` — `CONFIG_BT_CUSTOM=y` (what we must be able to
  NOT select on native_sim)
- Known constraints: process needs CAP_NET_ADMIN; adapter exclusively owned while
  running; host BT unavailable on that adapter

## Acceptance criteria

- [ ] Phone pairs + reconnects encrypted to the sample over the dongle
- [ ] Fork verification checklist answered
- [ ] Go/no-go decision recorded for tickets 0019–0024
