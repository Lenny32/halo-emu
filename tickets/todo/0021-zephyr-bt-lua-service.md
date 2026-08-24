# 0021 — Zephyr-BT Lua GATT service (REPL/data/video/audio over real BLE)

**Phase:** 2 — real BLE
**Depends on:** 0020
**Effort:** M

## Story

Port the custom Lua service to `bt_gatt_*` so a phone drives the emulator's REPL over
genuine GATT. The RX demux logic in `ble_lua.c` is transport-independent — lift it into
a shared function rather than rewriting it; the new backend becomes the third provider
of the `halo/ble_lua.h` contract (after hardware `ble_lua.c` and phase-1 TCP).

## Tasks

1. Extract `ble_lua_rx_demux()` from `modules/halo/src/ble_lua.c:241-312` (first-byte:
   0x01 → data ring, 0x02–0x07 → ctrl handler, else REPL text + `\n`) into a shared
   file both backends compile.
2. `modules/halo/src/native_ble/zh_lua.c` — `BT_GATT_SERVICE_DEFINE`:
   - Service `7A230001-5475-A6A4-654C-8431F6AD49C4`
   - RX `…0002` (Write + Write-Without-Response, `BT_GATT_PERM_WRITE_ENCRYPT`),
     TX `…0003` (Notify + CCC), Video `…0004` (Notify), Audio RX `…0005` (Write/WWR),
     Audio TX `…0006` (Notify) — encryption gating mirrors `SEC_LVL(WP, UNAUTH)`
   - Write callbacks → demux; TX/Video/Audio via `bt_gatt_notify_cb` with the existing
     per-char semaphore pattern
3. `halo_ble_lua_*` signatures unchanged → `lua_runtime.c`, `lua_bluetooth.c`,
   `luaport.h` untouched.
4. Throughput sanity: JPEG streaming on the Video char (notify pacing via the
   notify-complete callback, no busy retry loops).

## Key points in code

- `modules/halo/src/ble_lua.c` (893 lines) — reference semantics: ring buffers,
  blocking reads, notification flow control
- `applications/halo/PROTOCOL.md` — framing authority (data marker, ctrl codes, MTU)
- `modules/halo/include/halo/ble_lua.h` — frozen contract

## Acceptance criteria

- [ ] nRF Connect: write `print('hi')` + newline to RX → result notifies on TX
- [ ] Python suite M1 subset green over REAL BLE (brilliant-ble unmodified, no pyshim)
- [ ] Control codes 0x02–0x07 behave per LUA_RUNTIME.md over GATT
