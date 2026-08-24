# 0030 — Lua REPL bridge over the stub GATT (TCP 9563)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028
**Effort:** L

## Story

Expose the firmware's BLE Lua REPL on `tcp://127.0.0.1:9563` using the same wire protocol
the retired native_sim emulator served (`[u8 channel][u16 LE length][payload]`, MTU 512),
so scripts and the device test-suite can drive the emulated firmware. Transport path:
TCP client ⇄ QEMU-side bridge ⇄ 0028 doorbell/ring device ⇄ synthetic ROM GATT ⇄
`ble_lua.c` characteristics ⇄ Lua runtime.

## Tasks

1. QEMU-side bridge: connect the 0028 GATT doorbell device to a TCP chardev/socket on
   `127.0.0.1:9563` (loopback only — the REPL is arbitrary code execution). Map protocol
   channels to the GATT characteristics from `applications/halo/BLE_SERVICES.md` /
   `modules/halo/include/halo/ble_lua.h`: REPL RX (write w/o resp) / TX (notify), DATA
   channel, plus the control codes 0x02 (reboot: exit+relaunch semantics documented),
   0x03/0x04/0x06, MTU reporting 512 / `max_length()` 511, single-client policy.
2. Rewrite the test tooling (reference implementations recoverable from git tag
   `archive/native-sim`: `tools/repl_smoke.py`, `tools/run_emu_tests.py`, `pyshim/`):
   - `tools/repl_smoke.py` — REPL echo, MTU, data channel, single-client, control codes.
   - `tools/run_emu_tests.py` — launches `halo-emu` headless with a fresh `mram.img`,
     runs the unmodified device tests from a firmware checkout's
     `applications/halo/tests/` (path via `--tests-dir` or `HALO_FW_WS`), relaunch-on-exit
     = reboot semantics.
   - `pyshim/brilliant_ble` — the phone-library shim redirecting BLE calls to the TCP
     socket, so the upstream Python test suite runs unmodified.
3. Notify flow-control: GATT notifies from `ble_lua.c` must not drop when the TCP client
   stalls — bounded ring + backpressure on the doorbell device.

## Key points in code

- Firmware side is untouched and unmodified: `halo_ble_lua_repl_read/write`
  (`~/halo-firmware/alif/modules/halo/src/ble_lua.c:684/694`) already run against the
  stub GATT; the REPL thread is `modules/halo/src/lua_runtime.c:404-460`.

## Gate (acceptance)

- `tools/repl_smoke.py` passes end-to-end against `halo-emu -f zephyr.bin`.
- The M1 device-test subset (the set that was green on native_sim) passes via
  `tools/run_emu_tests.py` against a real firmware binary.
- Reboot control code relaunches cleanly with `/lfs` persistence intact.

## Implementation notes (done 2026-08-24)

Gate results: `tools/repl_smoke.py` all green against the 0.8.8 release
binary (echo, `max_length()` == 511, data channel both ways, single-client,
0x03/0x04/0x02 with `/lfs` intact across the reboot); M1 subset 6/6 via
`tools/run_emu_tests.py` against a locally built `zephyr.signed.bin`
(sysbuild, out-of-tree build dir — the firmware tree stays untouched).

- Bridge lives in `tools/ble_bridge.py`, a thread inside `halo-emu` (the
  "QEMU-side" of the story): TCP 9563 (`--repl-port`) <-> doorbell chardev
  over an internal unix socket with `wait=on`, so the boot-time GATT
  database dump is never lost.  Handles resolved from EVT_SVC/EVT_ATT by
  UUID; OP_CONNECT + CCC enables injected per client; a repeated service
  start-handle in the dump = guest reboot (drop client, re-arm).
  `--ble-port` keeps the raw doorbell TCP mode for tests/smoke_ble.py.
- The archive/native-sim git tag named in this ticket does not exist in any
  reachable repo; the wire protocol was reconstructed from the retired
  tickets 0005/0006 in the old workspace and the PyPI `brilliant-ble` 3.2.0
  API (mirrored by `pyshim/brilliant_ble`).
- MTU: `STUB_MTU` 247 -> 519 (`halo_ble_get_mtu()` subtracts the 7-byte
  GATT buffer header -> 512; `frame.bluetooth.max_length()` == 511); the
  stub's rwip_process payload buffer grew to 2+512 for full-MTU writes.
- Flow control: `stub_ipc_send` now kicks + waits instead of dropping when
  the G2H ring is full, and the QEMU device parks client frames + throttles
  the chardev when the H2G ring is full (`rx_deliver`/retry timer).  A
  stalled TCP client backpressures the firmware's notify path end to end.
- Reboot (0x02): on the Balletto `sys_reboot` is an SE service call —
  `halo_se.c` now handles SERVICE_BOOT_RESET_SOC/CPU (504/503) with a full
  machine reset, and `halo.c` zeroes ITCM/DTCM on system reset so
  `__noinit` state dies like hardware (`ble_lua`'s noinit magic otherwise
  makes the firmware skip GATT re-registration and BLE comes back dead).
  MRAM (mapped file) survives; no QEMU exit involved — but
  `run_emu_tests.py` still relaunches on the same MRAM if QEMU ever exits.
- Bugs found on the way, fixed at machine level: LPGPIO button pin
  (active-low) idled low and storm-fired its level interrupt (new
  `in-default` reset-level property, button idles high); pins configured
  `GPIO_INT_EDGE_BOTH` were treated as level-type because the Zephyr driver
  sets only `INT_BOTHEDGE` and leaves `INTTYPE_LEVEL` 0 (the DW IP gives
  INT_BOTHEDGE precedence — model follows).
- `halo-emu` auto-detects the vector-table offset (0x800 imgtool pad vs 0
  for a plain `west build`) instead of hardcoding 0x80020800.
- Caveat, not an emulator issue: the released 0.8.8.bin was built without
  git metadata, so `frame.GIT_TAG` is "" and `print(frame.GIT_TAG)` emits
  nothing (`lua_writeline()` is a no-op) — device tests that await that
  print hang on hardware with that binary too.  The M1 gate therefore runs
  against a firmware built from the workspace checkout
  (`west build -b halo --sysbuild alif/applications/halo`, build dir
  outside the tree), whose GIT_TAG is populated.
