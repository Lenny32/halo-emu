# 0029 — CDC200 display model: the UI window

**Phase:** 2 — QEMU machine emulation
**Depends on:** 0028 (nothing draws before `main()` unblocks)
**Effort:** L

## Story

The point of the emulator: see the UI. The CDC200 display controller continuously scans
out a fixed framebuffer — model it as a QEMU display device so the boot logo and every
Lua-drawn frame appear in the QEMU window (SDL/GTK), with `--headless` still supported.

## Tasks

1. CDC200 model @ `0x49031000` (compatible `tes,cdc-2.1`, driver
   `~/halo-firmware/zephyr/drivers/display/display_cdc200.c` + `.h` for offsets):
   - Registers used by the firmware: `GLB_CTRL@0x18` (**bit0 = enable — the on/off
     switch**), `SRCTRL@0x24` (shadow reload = commit point), `IRQ_MASK0/STATUS0/CLEAR0
     @0x34/0x38/0x3C` (bit0 = LINE irq), `LINE_IRQ_POS@0x40`, layer-1 block:
     `L1_PIX_FORMAT@0x11C` (=1 RGB888), **`L1_CFB_ADDR@0x134`** (halo programs
     **`0x58930000`** — global alias of CPU `0x20130000`, the static 256×256×3 `fb0_0`
     in `.alif_ns`), `L1_CFB_LENGTH@0x138` (pitch 768), `L1_CFB_LINES@0x13C` (256),
     window/blend/alpha regs `0x10C–0x128`, timing regs `0x08–0x14`.
   - Scanout: while `GLB_CTRL` bit0 is set, present the buffer at the programmed CFB
     address (translate the `0x58...` alias) to the QEMU console at the panel rate
     (~30–60 Hz is fine; pixel clock is 8.76 MHz for 256×256); RGB888 → host surface.
   - Raise the **scanline IRQ 333** at `LINE_IRQ_POS` each frame when unmasked — the
     driver's double-buffer commit path (`cdc200_swap_fb`) relies on it.
   - Disable = blank (the firmware's power-save drives enable/disable; splash does
     init→enable→draw→disable, holding ~3 s).
2. DSI/DPHY happy-path fakes: DSI @ `0x49032000` (IRQ 343, `snps,designware-dsi`) and
   D-PHY @ `0x4903F000`/`0x49033000` — return `PHY_LOCK` + stop-state bits in
   `DSI_PHY_STATUS` so `mipi_dsi_attach()` succeeds (`dphy_dw.c:462-490,605-617` are
   bounded polls; without the fake the splash is skipped — non-fatal but defeats this
   ticket).
3. I2C1 targets (on the 0026 DW I2C controller): **vga020 panel @ `0x54`** — ack writes
   (the driver writes reg `0x6C00=0x00` at hw-init), and **TPS65132 display PMIC @
   `0x3E`** — ack the rail programming. GPIO-driven panel reset/regulator enables already
   land in the 0026 GPIO model.
4. `--headless`: QEMU `-display none`; add `halo-emu --screenshot <png>` passthrough
   (QMP `screendump`) for CI and quick checks.

## Key points in code

- Framebuffer ground truth: `fb0_0` @ CPU `0x20130000`, 0x30000 bytes, section `.alif_ns`
  (verified in `zephyr.elf` symtab); firmware programs the **global alias** `0x58930000`.
- Panel bring-up order: `vga020_hw_init()` (lazy, first power-save(false)/boot-logo) —
  regulators VDD→VPOS→reset→VENG, 100 ms, `mipi_dsi_attach()`, I2C reg write; failure is
  clean `-EIO` rollback (`alif/drivers/display/display_vga020.c`).

## Gate (acceptance)

- `halo-emu -f zephyr.bin` opens a 256×256 window showing the boot-logo splash (~3 s),
  matching the device behavior.
- After boot, frames drawn by the running firmware update live; blank/unblank follows
  `GLB_CTRL` bit0.
- `--headless` boots clean; `--screenshot` captures the current frame.
