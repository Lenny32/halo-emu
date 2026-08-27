# 0028 — Synthetic BLE/LC3 ROM stub (unblock `main()`)

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0027
**Effort:** XL — the risk item of the queue

> **Superseded in part (2026-08-26):** the "declared-unsupported" tier this ticket
> defined for LE Audio is implemented by tickets 0038 (GAF profile layer + ASE state
> machine) and 0039 (isochronous data path). The `Error 255` / `Failed to initialize
> LE Audio service` pair recorded here as an accepted outcome no longer appears.

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

## Implementation notes (done 2026-08-24)

Reworked against the environment actually available (the firmware reference
tree lives at `~/Projects/halo-firmware`, has **no build directory** and no
`zephyr.map`, and no firmware binary exists anywhere on this host):

1. **Symbol audit without the map**: intersected the 983+10 symbol names from
   the vendored `rom_symbols_{ble,lc3}.lds` v1_2 maps with the identifiers used
   by the compiled-into-app sources (`modules/halo/src/*.c`,
   `modules/hal/alif/ble/plf/*.c`) → **79 referenced entry points**; the
   remaining 914 became trap thunks. Call-flow semantics were derived by
   reading `alif_ble.c`, `ble_connection.c`, `ble_security.c`, `ble_service.c`,
   `ble_manager.c`, `ble_lua.c`, `ble_battery.c`, `ble_ota.c`, `ble_ancs.c`,
   `ble_audio.c`.
2. **Deliverables** (all inside this repo):
   - `rom-stub/` — the stub: generated 4-byte `b.w` veneers at every pinned
     address (`gen_rom_layout.py`), semantic GAPM/GAPC/GATT/co_buf
     implementations, declared-unsupported LE-Audio/LC3 tier, per-symbol trap
     thunks reporting symbol index + caller LR. Vendored Alif v1_2 headers +
     symbol maps under `rom-stub/vendor/` (declarations only). Output
     `rom-stub-v1_2.bin` + `rom-stub-v1_2.syms`.
   - QEMU: `hw/arm/halo_ble.c` doorbell device (shared rings in the ROM
     window, chardev bridge, trap decoding via the `.syms` table, fake
     UTIMER0 channel page whose capture-A IRQ 377 is the host→guest signal —
     exactly the IRQ `plf/sync_timer.c` connects); `halo.c` gained `rom-file=`
     and `ble-symfile=` machine options.
   - `halo-emu` loads the stub automatically and serves the doorbell bridge
     on `tcp://127.0.0.1:9564` (`--ble-port`).
   - Toolchain: `init.sh` fetches a pinned xPack `arm-none-eabi-gcc` into
     `deps/toolchain` when the system has none, and builds the stub.
   - Pairing is emulated deterministically (Just-Works, fabricated peer keys
     with the peer address as identity) so `halo_ble_is_paired()` becomes
     true and the Lua RX write gate opens; `ble_stack_init` returns nonzero
     because the firmware-side `alif_ble.c` treats the result as a boolean
     (the header's `BLE_INIT_ERR_NONE = 0` contradicts its own consumer).
3. **Gate — run literally with a synthetic image** (no `zephyr.bin` exists on
   this host; see repo memory "no-firmware-build-on-host"):
   `rom-stub/test/fw_blesmoke.c` re-enacts the firmware's exact ROM call
   sequence (hook table → `ble_stack_init`/`rwip_init` → GAPM configure/name →
   GATT service add → adv create/data/scan-rsp/start → `rwip_process` event
   loop), compiled against the same vendored headers and linked against the
   same pinned addresses via the `.lds` maps. `tests/smoke_ble.py` evidence:
   - boot markers `stack-init-ok`, `gapm-ok`, `svc-hdl 0010`, `adv-start`,
     `ready` — **zero trap hits on the boot path**;
   - deliberate call to unimplemented `gapm_get_token_id` → QEMU logs
     `ROM stub trap: gapm_get_token_id (symbol #498) called from
     LR=0x800214df` and the call returns `GAP_ERR_NOT_SUPPORTED`;
   - doorbell CONNECT → `connected`/`paired`/`encrypted`; GATT write to the
     CCC then to the RX value reaches the app's `cb_att_val_set` and the
     echoed notification arrives back over the bridge — the exact transport
     ticket 0030 needs.
   **Follow-up:** re-run the gate against a real `zephyr.bin`
   (`halo_lua_runtime_init` + `boot_write_img_confirmed()` reached) once a
   firmware build or release artifact is available (ticket 0034's `--fetch`).
