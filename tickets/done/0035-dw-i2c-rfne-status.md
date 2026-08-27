# 0035 — DW I2C: model the FIFO status bits (RFNE) so real drivers read

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0026 (DW I2C controllers), 0029 (I2C1 register-file targets)
**Effort:** S

## Story

Booting a real `zephyr.bin` (`./halo-emu -f 0.8.8.bin`) fails every I2C **read**:

```
<wrn> regulator_tps65132: I2C read failed (reg=0x00, ret=-5), retries left: 2
<err> regulator_tps65132: I2C read failed after 3 retries (reg=0x00)
<err> regulator_tps65132: TPS65132 hw init failed: -5
```

The device side is correct — I2C1 (`designware-i2c` @`0x49011000`, IRQ 133,
`patches/files/hw/arm/halo.c:450`) exists and the TPS65132 stand-in is attached at
`0x3E` (`halo.c:457-473`, `halo_i2c_regfile.c`), matching the firmware DT
(`~/halo-firmware/build/halo/zephyr/zephyr.dts:420-451`). The bug is in the
**upstream QEMU controller model**, `hw/i2c/designware_i2c.c` (v11.1.0):

- `DW_IC_STATUS` is declared `.reset = 0x6` (TFE|TFNF), `.ro = 0xffffffff`, and the
  only bit ever updated at runtime is `ACTIVITY`. **`RFNE` (bit 3) reads 0 forever**,
  even with bytes in `rx_fifo` (the push site bumps `RXFLR` and `RX_FULL` only).
- Zephyr drains RX with `while (test_bit_status_rfne(...) && dw->xfr_len > 0)`
  (`~/halo-firmware/zephyr/drivers/i2c/i2c_dw.c:267-296`, bit at
  `i2c_dw_registers.h:215,218`). With RFNE low the loop body never runs, `xfr_len`
  stays > 0, and after a clean STOP the driver hits
  `if (dw->xfr_len > 0) { ret = -EIO; }` (`i2c_dw.c:806-809`) — the deterministic
  `-5` on all three retries.
- Writes work only by accident: the TX path gates on `TFNF`, permanently 1 from the
  reset value. Hence "I2C **read** failed" specifically.
- Why 0029's gate missed it: `rom-stub/test/fw_dispsmoke.c:130-134` polls `IC_RXFLR`
  instead of `RFNE` (it `#define`s `STATUS_RFNE` at :87 and never uses it), so it
  sidesteps the exact bit the real driver depends on. 0029 was gated before a real
  `zephyr.bin` existed on this host.

## Tasks

1. Overlay a patched controller model as **`patches/files/hw/i2c/designware_i2c.c`**
   (verbatim copy of QEMU v11.1.0 + the changes below; `init.sh:114-118` copies every
   file under `patches/files/` into the pinned `qemu/` tree by path, so no change to
   `patches/qemu-build-integration.patch` is needed).
2. Add `dw_i2c_update_status(s)` and call it at every RX FIFO mutation:
   - `RFNE = !fifo8_is_empty(&s->rx_fifo)`, `RFF = fifo8_is_full(...)`;
   - `TFE`/`TFNF` held at 1 (the TX FIFO is not modelled — commands go onto the bus as
     soon as they are written);
   - call sites: push (`dw_ic_data_cmd_reg_post_write` receive branch), pop
     (`dw_ic_data_cmd_reg_post_read`), `dw_i2c_reset_to_idle()`, and
     `designware_i2c_enter_reset()`.
3. Two latent divergences in the same file, fixed while there:
   - `IC_ENABLE.TX_CMD_BLOCK` was treated as a user abort (`dw_ic_tx_abort(USER_ABRT)`);
     that bit only holds commands in the TX FIFO — make it a no-op, abort on `ABORT`
     only.
   - `dw_ic_tx_tl_reg_pre_write` logged `LOG_GUEST_ERROR` for `ic_tx_tl > 15`; the DT
     sets `tx_threshold = <0x10>` (16, `i2c_dw.c:1339`, `tx_tl` is `uint8_t`) and Zephyr
     writes it verbatim, so every controller init logged a spurious guest error. Clamp
     the FIFO-depth value silently; keep the log for genuinely out-of-range values.
4. Unrelated cosmetic, bundled: `tools/ble_bridge.py:_resolve_handles()` logged
   "Lua service resolved" 6× per boot because it runs on every `EVT_ATT` and logged
   whenever the handle dict *grew* (TX_CCC, VIDEO_VAL/CCC, AUDIO_RX/TX_VAL,
   AUDIO_TX_CCC arrive after rx/tx are known) while the printed rx/tx never change.
   Log only when the printed `rx`/`tx` pair changes.

## Not in scope

- `halo_ble_audio: Unable to configure BAP unicast server! Error 255` +
  `Failed to initialize LE Audio service: -1 (continuing)` — **by design**, the ROM
  stub declares the whole GAF/BAP/ISO tier unsupported (`rom-stub/src/stub_misc.c:18-33`,
  `GAF_STUB(bap_uc_srv_configure)` returns `0xFF`); accepted outcome of ticket 0028
  (see `0028-synthetic-ble-rom-stub.md:44-45`).
- `littlefs: Corrupted dir pair ... can't mount (LFS -84); formatting` — **expected on
  first boot only**, the launcher had just created a blank `mram.img`; it mounts clean
  once the image persists.
- PAG7982 camera @`0x40` on I2C1 is still absent (DT has it enabled, so its probe
  NACKs) — that belongs to ticket 0033.

## Gate

- `./build.sh`, then `./halo-emu -f 0.8.8.bin`: no `regulator_tps65132: I2C read
  failed` / `hw init failed` lines, no `invalid setting to ic_tx_tl`, and
  "Lua service resolved" printed once.
- Second boot on the persisted `mram.img`: littlefs mounts without formatting.
- `tests/smoke_display.py` (0029's RXFLR-polling path) and `tests/smoke_ble.py` still
  pass.

## Outcome (done 2026-08-26)

1. **`patches/files/hw/i2c/designware_i2c.c`** — new overlay (verbatim QEMU v11.1.0
   copy + 6 marked changes; the pinned `qemu/` tree had no local delta on this file, so
   the copy is a clean base):
   - `dw_i2c_update_status()` drives `RFNE`/`RFF` from the FIFO and pins `TFE`/`TFNF`
     high; called from the push site, the pop site, `dw_i2c_reset_to_idle()` and
     `designware_i2c_enter_reset()`.
   - `IC_ENABLE.TX_CMD_BLOCK` no longer aborts the transfer.
   - `ic_tx_tl == 16` (the DT value) clamps silently; `> 16` still logs.
2. **`tools/ble_bridge.py`** — `_resolve_handles()` compares the printed `rx`/`tx` pair
   instead of the whole dict.
3. **Gate — run against the real `0.8.8.bin`** (first firmware image available on this
   host, so this is also the first real-Zephyr exercise of the I2C read path; closes
   0029's "re-run with a real zephyr.bin" follow-up for the PMIC leg):
   - `./halo-emu -f 0.8.8.bin --headless`: **no `regulator_tps65132` warnings or
     errors at all** (previously 6 warnings + 4 errors per boot), no littlefs format
     on the persisted `mram.img`, `Lua service resolved` printed **once** (was 6×).
   - `--screenshot` at t=2 s: the boot splash renders (logo + "Halo 11"). This is the
     decisive check — `regulator_tps65132_hw_init()` runs from
     `regulator_tps65132_enable()`, so the panel only lights up if the I2C
     read-compare-write voltage programming succeeded.
   - `tests/smoke_display.py`, `tests/smoke_ble.py`, `tests/smoke_controls.py`: pass.
   - `tests/smoke_audio.py`: 21/21 pass (3 consecutive isolated runs). It failed once
     on "microphone amplitude matches the injection" when launched immediately after
     `smoke_controls.py` (which reboots the guest via `wdt-fire`); unrelated to I2C —
     pre-existing back-to-back flake, worth a look if it recurs.
   - `halo_ble_audio: ... Error 255` is unchanged, as intended (see *Not in scope*).
