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
