# 0028 — Synthetic BLE/LC3 ROM stub (unblock `main()`)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0027
**Effort:** XL — the risk item of the queue

## Story

The firmware's BLE host stack lives in on-chip ROM: the app links against **983 absolute
symbols** (`~/halo-firmware/modules/hal/alif/ble/v1_2/rom_symbols_ble.lds`, plus 10 LC3
symbols in `lc3/v1_2/rom_symbols_lc3.lds`, spanning ~`0x0009F000–0x00141000`) and calls
them directly (`BL` into ROM). We have no device/SWD access, so the real ROM can never be
dumped — **the permanent strategy is a synthetic ROM**: our own C code, compiled to those
exact addresses, implementing only the API surface the app actually uses. Without it,
`main()` blocks forever in `alif_ble_enable()` and splash/display/Lua never start.

## Tasks

1. Symbol audit: extract from `~/halo-firmware/build/halo/zephyr/zephyr.map` the subset of
   ROM symbols the image actually references (map shows resolved `BL` targets; expect a few
   dozen GAPM/GATT/co_* entry points plus `ble_stack_init`=0x140d55, `rwip_init`=0xbc0a9,
   `rwip_process`=0xbc0d9 and the LC3 encode/decode set). Vendor the needed API
   *declarations* (structs, enums, prototypes) from `~/halo-firmware/modules/hal/alif/ble`
   headers into `rom-stub/include/` — declarations only, no Alif implementation code.
2. `rom-stub/`: C sources + linker script placing each implemented function at its pinned
   `.lds` address (section-per-symbol + explicit placement; unimplemented symbols get a
   trap-and-log thunk so any missed call is loud, not silent). Toolchain:
   `gcc-arm-none-eabi` (apt), Thumb-2, no libc. Output: `rom-stub.bin` loaded by the
   machine into the ROM region at startup (`halo-emu` passes it automatically).
3. Behavior (driven by what `~/halo-firmware/alif/modules/halo/src/ble_connection.c`,
   `ble_lua.c`, `ble_battery.c`, `ble_audio.c`, ota/SMP and
   `modules/hal/alif/ble/plf/alif_ble.c` call):
   - `ble_stack_init`/`rwip_init`/`rwip_process` loop minimal scheduler: invoke the app's
     registered callback `cb_on_stack_initialised` → releases `rwip_init_sem` → `main()`
     unblocks. (The `__ASSERT` paths in `alif_ble.c:198-228` must be satisfied: HCI UART
     init is bypassed at this layer — the stub never touches `uart_hci`.)
   - GAPM: device config, address (serve a fixed EUI48), advertising start/stop → return
     success; record state in stub RAM (scratch inside the ROM region or `.alif_ns`).
   - GATT: accept service/characteristic registration (Lua RX/TX, battery, audio, OTA/SMP
     per `applications/halo/BLE_SERVICES.md`), store attribute table, deliver write/notify
     callbacks. Expose a **doorbell MMIO page + shared ring** (small custom device in the
     machine model) so the host side (QEMU) can inject GATT writes and collect notifies —
     that is the transport ticket 0030 builds the Lua REPL bridge on.
   - LC3 ROM entry points: return "not supported" errors (audio streaming is out of scope
     until 0032; ble_audio init failure is LOG_WRN + continue).
   - BLE sync timer (`plf/sync_timer.c`, UTIMER0 ch @ `0x48001000`, EVTRTR `0x400E2000`,
     IRQs 377/384): tolerate register access; the stub does not need real timing.
4. Version pinning: the stub is tied to ROM symbol map v1_2 (`CONFIG_ALIF_BLE_ROM_IMAGE_V1_2`).
   Name artifacts accordingly (`rom-stub-v1_2.bin`); a firmware built against a different
   ROM map needs a matching stub build — detect mismatch heuristically (log the addresses
   of the first trap hits).

## Key points in code

- Blocker being removed: `alif_ble_enable()` `k_sem_take(&rwip_init_sem, K_FOREVER)`
  (`~/halo-firmware/modules/hal/alif/ble/plf/alif_ble.c:256`).
- ES0/SE interplay: `take_es0_into_use()` (`es0_power_manager.c:151`) runs SE calls
  GET_TOC_VERSION(200) + EXTSYS0_BOOT(800) before stack init — already satisfied by the
  0026 responder; `alif_eui48_read` falls back to SE RND — fine.
- App-side consumers: `halo_ble_conn_init` (`modules/halo/src/ble_connection.c:260`),
  REPL rings drained by `lua_runtime.c:404-460` via `halo_ble_lua_repl_read/write`
  (`ble_lua.c:684/694`).

## Gate (acceptance)

- `halo-emu -f zephyr.bin` completes `main()`: boot proceeds through splash
  (display may still be headless-stubbed) to `halo_lua_runtime_init`, and
  `boot_write_img_confirmed()` is reached (log/gdb evidence).
- No trap-thunk hits during a plain boot; any hit is logged with symbol name + caller PC.
- A GATT write injected via the doorbell device reaches `ble_lua.c`'s write callback
  (smoke evidence for ticket 0030).
