# Halo emulator — target UX and hardware reference

**Status: rebuilding.** The QEMU-based emulator is being implemented ticket by ticket
(see `README.md`); nothing is runnable until ticket 0026 lands. The retired native_sim
emulator lives at git tag `archive/native-sim`.

## Target UX

```sh
./init.sh                                  # once: fetch + build the QEMU fork (ticket 0025)
./halo-emu -f zephyr.bin                   # run a firmware (built or downloaded)
./halo-emu -f zephyr.signed.bin            # signed images load identically
./halo-emu -f fw.bin --flash mram.img      # named persistent MRAM (default ./mram.img)
./halo-emu -f fw.bin --flash-erase         # factory reset the data partition
./halo-emu -f fw.bin --headless            # no window (CI); --screenshot for captures
./halo-emu --fetch 0.8.8                   # download + run a GitHub release (ticket 0034)
```

The Lua REPL is served on `tcp://127.0.0.1:9563` (same wire protocol as before:
`[u8 channel][u16 LE length][payload]`, MTU 512) from ticket 0030.

---

# Hardware reference — Alif Balletto B1 as the halo firmware uses it

Distilled from the real build artifacts (`~/halo-firmware/build/halo/zephyr/{.config,
zephyr.dts,zephyr.map}`, firmware 0.8.8 @ d1a9645). `~/halo-firmware` is a read-only
reference; per-ticket details live in the ticket files.

## CPU / core

Cortex-M55 RTSS-HE, ARMv8.1-M + MVE (int+float), FPU hard-ABI, I/D-cache enabled in
`SystemInit`, runs in the **Secure** state and programs SAU itself (one NS region
`0x200E0000–0x2017FFFF`). NVIC: 480 external IRQs, 8 priority bits. Watchdog fires as NMI.
Ethos-U55 present in DT but **unused** by the firmware (`CONFIG_ARM_ETHOS_U` off).
Boot-time must-tolerate: TGU LUT loops (ITGU `0xE001E500` / DTGU `0xE001E600` — `CFG@+4`
must read a sane BLKSZ), CGU/AON scratch writes (`0x1A60xxxx`, `0x4902F008`,
`0x43007000..10`, `0x400E2080`, `0x4300A00C/B00C`).

## Memory map

| Region | Base | Size | Notes |
|---|---|---|---|
| MRAM (flash) | `0x80000000` | 2 MB | memory-mapped, **writable**, no controller; erase 1 KB / write 16 B |
| ITCM | `0x00000000` | 512 KB | alias `0x58000000` |
| BLE/LC3 ROM | ~`0x0009F000–0x00141000` | — | 983+10 pinned symbols; synthetic stub (ticket 0028) |
| DTCM (`zephyr,sram`) | `0x20000000` | 1.5 MB total (kernel RAM at `0x2000A000`) | alias `LOCAL_TO_GLOBAL(x)=x-0x20000000+0x58800000` |
| `.alif_ns` | `0x200E0000` | 512 KB | NS shared RAM; holds the framebuffer |

MRAM partitions: mcuboot `0x0`+0x20000 · **image-0 `0x20000`+0xC3000** · image-1
`0xE3000`+0xC3000 · scratch `0x1A6000`+0x2000 · **storage (littlefs `/lfs`)
`0x1A8000`+0x20000** · SE-reserved `0x1C8000`+0x38000.

Boot: app loads at `0x80020000`, vector table at `0x80020800`
(`CONFIG_ROM_START_OFFSET=0x800` — signed images carry the imgtool header in that pad).
mcuboot (`0x80000000`) is optional to emulate; the app boots standalone.

## Boot sequence and fatal dependencies

`main()` order: pm → mem → wdt → led → battery → file → **ble** → splash → lua_runtime.
`CONFIG_ASSERT=y`: init failures are kernel panics.

1. **SE services over MHUv2** — rx `0x40040000`/IRQ 37, tx `0x40050000`/IRQ 38, channel 0.
   Payload = global-alias pointer written to `CH_SET`; SE fills the struct in place.
   Ack-and-zero passes the boot calls (HEARTBEAT 0, SET_RUN 311, GET/SET_OFF 312/313,
   TOC 200, EXTSYS0_BOOT 800, RND 400 wants real bytes). Unanswered ⇒ minutes of busy-spin
   then fatal assert in `halo_pm_init`.
2. **VBAT/ADC** `0x49020000` (+`0x49023000`, `0x1A604000`), ch 4, IRQs 151/154/157/160 —
   `-ENODEV` fatal. **UTIMER3 PWM LED** `0x48004000` — `-ENODEV` fatal.
3. **Watchdog** `arm,cmsdk-watchdog` `0x40100000`, 160 MHz, 5 s window, fed 1 Hz — stub as
   never-firing.
4. **DW APB RTC** `0x42000000`, 32768 Hz, IRQ 58 — the tickless-idle timer; required.
5. **littlefs on MRAM storage** — mount→auto-format→mount; fatal only if MRAM not writable.
6. **BLE**: `main()` blocks forever in `alif_ble_enable()` (`k_sem_take(K_FOREVER)`) until
   the ROM host stack signals up — splash/display/Lua all come after. Host stack = on-chip
   ROM called via pinned addresses (`modules/hal/alif/ble/v1_2/rom_symbols_ble.lds`);
   controller = ES0 RISC-V core reached by HCI-H4 over `uart_hci` `0x4300A000` (IRQ 50,
   3 Mbaud). The synthetic ROM stub bypasses HCI/ES0 entirely.

## Console

`ns16550` UART3 @ `0x4901B000`, reg-shift 2, IRQ 127, 115200, refclk 40 MHz; DesignWare
`DLF` register at offset `0xC0` must accept writes. Deferred logging, 8 KB buffer.

## Display

CDC200 (`tes,cdc-2.1`) @ `0x49031000`, scanline IRQ 333: continuous scanout, layer 1,
RGB888 256×256, framebuffer fixed at bus `0x58930000` (= CPU `0x20130000`, `fb0_0` in
`.alif_ns`), pitch 768. Enable = `GLB_CTRL@0x18` bit0; commit = `SRCTRL@0x24` shadow
reload. Panel vga020 (DSI, I2C ctrl @ i2c1 addr `0x54`, lazy-init) + TPS65132 PMIC (`0x3E`);
DSI `0x49032000` / D-PHY `0x4903F000` need only `PHY_LOCK`/stop-state fakes (bounded
timeouts; failure skips the splash, non-fatal).

## Other peripherals (lazy-init, non-fatal)

I2C0 `0x49010000`/IRQ 132: BMA580 IMU `0x18`, QMC6308 mag `0x2C`. I2C1 `0x49011000`/IRQ
133: vga020 `0x54`, TPS65132 `0x3E`, PAG7982 camera `0x40`. Audio out: `alif,i2s-sync`
`0x49014000`/IRQ 141 (ISR in DTCM) + MAX98357A (gpio8.5). Mic: LPPDM `0x43002000`/IRQ 49,
DMA PL330 `0x400C0000` (IRQs 0-4,32) ch4. Camera: LPCAM `0x43003000`/IRQ 54 (parallel
8-bit). GPIO: DW banks gpio0–9 `0x49000000..0x49009000` (IRQs 179–253), LPGPIO
`0x42002000` (IRQs 171/172) — **button = LPGPIO pin 1, active-low**. Pinctrl `0x1A603000`/
`0x42007000` (direct writes, no SE). BLE sync timer UTIMER0 `0x48001000` + EVTRTR
`0x400E2000` (IRQs 377/384). Disabled in DT (not needed): all other UARTs, SPI, OSPI/XIP,
SD, CAN, I3C, DAC, second ADC.
