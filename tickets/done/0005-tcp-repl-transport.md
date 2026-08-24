# 0005 — TCP REPL/data transport + BLE stubs (transport seam)

**Phase:** 1 — core emulator
**Depends on:** 0003, 0004
**Effort:** L — the milestone-1 ticket

## Story

The Lua REPL is BLE-only on hardware. The emulator replaces the GATT channel with a TCP
socket speaking the same PDU semantics, cut at the **existing `halo/ble_lua.h` symbol
set** via link-time polymorphism: exactly one provider is linked — hardware `ble_lua.c`,
emulator `transport_tcp.c`, or (phase 2) a Zephyr-BT backend. This seam is the future
real-BLE hook; design it as a documented contract, not an ad-hoc stub.

## Tasks

1. `emulator/module/include/halo/lua_transport.h` — documents the contract
   (includes `ble_lua.h`; same `<halo/...>` include path, injected by the
   module — AGENTS.md forbids files under `modules/halo/`): PDU-oriented, blocking reads with `k_timeout_t`, first-byte
   demux (0x01 → data channel; 0x02–0x07 → control codes; else REPL text), MTU-bounded
   writes, one client at a time.
2. `emulator/module/src/transport_tcp.c` (Zephyr context) — implements the full
   `halo_ble_lua_*` set: `init/deinit`, `repl_read/repl_write`, `data_read/data_write`,
   `audio_read/audio_write`, `video_write`, `register_ctrl_handler`. Reuse the same
   k_msgq/ring-buffer machinery as `ble_lua.c`.
3. `emulator/module/src/transport_tcp_bottom.c` (native-simulator host context) —
   BSD sockets + pthread accept/read/write, bytes handed across via the nsi top/bottom
   pattern. No Zephyr net stack dependency.
4. Wire framing (TCP is a stream; GATT gave PDU boundaries — recreate them):
   `[u8 channel][u16 le length][payload]`, port `CONFIG_HALO_EMU_TCP_PORT` (9563).
   Channel 0 = Lua RX/TX (payload = exact GATT PDU), 1 = audio, 2 = video.
   One frame = one PDU; `repl_read` returns at most one PDU; payloads > MTU rejected
   with `-EMSGSIZE` (hardware honesty). Second client connection refused (mirrors
   single-connection BLE policy).
5. `emulator/module/src/ble_stubs.c` — what `lua_bluetooth.c`, `main.c`,
   `battery_manager` need with `CONFIG_HALO_BLE_MANAGER=n`:
   - `halo_ble_init` → start TCP listener
   - `halo_ble_is_connected` → socket state
   - `halo_ble_get_mtu` → `CONFIG_HALO_EMU_MTU` when connected, 0 otherwise
     (so `frame.bluetooth.max_length()` = 511, matching PROTOCOL.md MTU 512)
   - `halo_ble_get_address` → fake EUI-48 (stable, e.g. hostname-derived; drives the
     "Halo XX" name convention)
   - `halo_ble_conn_prepare_reboot`, `register/unregister_callback` (connect/disconnect
     events fired from accept/close)
6. Control code 0x02 (reboot): `sys_reboot` on posix → exit; provide a restart wrapper
   note in `EMULATOR.md`. 0x03 interrupt / 0x04 restart / 0x05 reset / 0x06 exit /
   0x07 remove-all flow through `lua_ctrl_handler` unchanged.

## Key points in code

- `modules/halo/include/halo/ble_lua.h` — THE contract; do not change signatures
- `modules/halo/src/ble_lua.c:241-312` — reference RX demux semantics to replicate
- `modules/halo/src/lua_runtime.c:83` (`lua_ctrl_handler`), `:~452`
  (`halo_ble_lua_repl_read` loop) — consumers, untouched
- `modules/halo/include/luaport.h:33` — `lua_writestring → halo_ble_lua_repl_write`;
  the link-time seam routes Lua `print()` to the socket with zero changes
- `applications/halo/PROTOCOL.md` — wire format authority
- `applications/halo/LUA_RUNTIME.md` — control-code behaviour

## Acceptance criteria

- [x] `emulator/build.sh` (native_sim_64) links and boots to the Lua REPL
- [x] Framed `print('hi')` on channel 0 echoes the result (`tools/repl_smoke.py`)
- [x] 0x03 interrupts a busy loop; 0x04 restarts the VM; 0x06 exits the runtime
- [x] `frame.bluetooth.max_length()` returns 511; oversize writes chunk to MTU-sized
      PDUs exactly like hardware `send_notification()` (single-PDU >MTU: `-EMSGSIZE`)

## Note from ticket 0003 (done)

- The exact undefined-symbol list this ticket must provide (verified against the
  0003 build): `halo_ble_lua_{repl_read,repl_write,data_read,data_write,
  register_ctrl_handler}`, `halo_ble_{is_connected,get_mtu,get_address,
  conn_disconnect}`, and `halo_ble_sec_pairing_window_open` (add it to the
  ble_stubs.c set — lua_button.c's pairing gesture; a no-op success is fine
  until real BLE, phase 2).
- Implementation files go in `emulator/module/src/` (AGENTS.md hard rule), not
  `modules/halo/src/emu/`; `halo_ble_get_address` can reuse se_stubs.c's fixed
  EUI (2C:F7:F1:E3:00:00:00:01) for a stable "Halo XX" name.
- Control code 0x02: `sys_reboot()` already exits the process via
  `module/src/misc_stubs.c` (`CONFIG_REBOOT=y`); only the EMULATOR.md restart-
  wrapper note remains.

## Implementation notes (done 2026-08-24)

- All files under `emulator/module/` per the AGENTS.md hard rule (paths above
  reworked accordingly); wire/channel constants live Zephyr-free in
  `src/transport_tcp_bottom.h` so both build contexts share them.
- `HALO_TRANSPORT_TCP` now `depends on !HALO_BLE_MANAGER` (one ble_lua.h
  provider per image) and `select RING_BUFFER` (on hardware the BLE manager
  pulls it in). Listener binds **loopback only** — the REPL is arbitrary code
  execution.
- Bottom hands frames to the Zephyr pump thread through a one-frame
  mutex/condvar mailbox: the kernel TCP receive buffer is the queue, so a slow
  consumer backpressures the client. Pump polls at 1 ms, priority 6 (above the
  REPL/data threads at 7).
- `SYS_INIT(APPLICATION)` starts the listener: with `CONFIG_HALO_BLE_MANAGER=n`
  nothing calls `halo_ble_init()` → `halo_ble_lua_init()` (ble_stubs'
  `halo_ble_init` still routes there, idempotent). Static ring buffers — no
  `halo_mem_init()` ordering dependency.
- **Two native_sim discoveries** (this was the first time the app ever ran):
  1. `src/setjmp_x86_64.S`: picolibc ships no x86-64 setjmp, so the link bound
     host glibc's mask-saving setjmp (~200-byte jmp_buf) against picolibc's
     64-byte type — smashing `lua_longjmp.status` on the stack and hanging
     `lua_newstate()` in `luaD_closeprotected()` forever. The module now ships
     a matching 64-byte-layout setjmp/longjmp.
  2. Control codes during a busy Lua loop: a CPU-bound thread freezes
     native_sim's simulated time, so no Zephyr thread (the pump included) can
     deliver 0x03 — unlike the hardware BLE ISR. The bottom's host thread
     calls `emu_tcp_ctrl_notify()` (NATIVE_SIMULATOR_IF) which asynchronously
     installs a Lua hook (the async-safe `lua_sethook` pattern); the hook
     k_sleep()s in the REPL thread, sim time advances, the pump dispatches the
     control code through the normal path.
- `halo_ble_sec_pairing_window_open` / `conn_disconnect` /
  `conn_prepare_reboot` / `register_callback` (+ connect/disconnect event
  dispatch) live in ble_stubs.c as per the 0003 note; EUI-48
  `2C:F7:F1:00:00:01` derives from se_stubs' fixed EUI-64.
- Acceptance verified end-to-end by `emulator/tools/repl_smoke.py`
  (11 checks incl. `--reboot`); EMULATOR.md carries the restart-wrapper note.
