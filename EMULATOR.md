# Halo emulator — target UX and hardware reference

**Status: rebuilding.** The QEMU-based emulator is being implemented ticket by ticket
(see `README.md`). As of ticket 0032 the real firmware boots all the way through: console
banner + logs, littlefs mounted, BLE up on the synthetic ROM stub, the 256×256 UI in a
window, the Lua REPL on TCP 9563, the runtime controls (button, battery, charger, LED,
reboot, watchdog) on TCP 9562, and audio both ways — speaker, microphone and a working
LC3 codec — `./halo-emu -f zephyr.bin`. Remaining: camera (0033), CI + firmware fetch
(0034). The retired native_sim emulator lives at git tag `archive/native-sim`.

## Target UX

```sh
./init.sh                                  # once: fetch + build the QEMU fork (ticket 0025)
./halo-emu -f zephyr.bin                   # run a firmware (built or downloaded)
./halo-emu -f zephyr.signed.bin            # signed images load identically
./halo-emu -f fw.bin --flash mram.img      # named persistent MRAM (default ./mram.img)
./halo-emu -f fw.bin --flash-erase         # factory reset the data partition
./halo-emu -f fw.bin --headless            # no window (CI); --screenshot for captures
./halo-emu -f fw.bin --ctl-port 9562       # control socket (ticket 0031; 0 disables)
./halo-emu -f fw.bin --wav-out spk.wav     # record the speaker (ticket 0032)
./halo-emu -f fw.bin --wav-in voice.wav    # feed the microphone from a file
./halo-emu -f fw.bin --audio               # live host playback/capture (SDL; try pa)
./halo-emu --fetch 0.8.8                   # download + run a GitHub release (ticket 0034)
```

The Lua REPL is served on `tcp://127.0.0.1:9563` by default (ticket 0030,
`--repl-port`; same wire protocol as the retired native_sim emulator:
`[u8 channel][u16 LE length][payload]`, channel 0 = REPL/data PDUs, 1 = audio,
2 = video; MTU 512, `frame.bluetooth.max_length()` = 511; loopback only, one
client at a time). The bridge (`tools/ble_bridge.py`, running inside
`halo-emu`) owns the doorbell chardev over an internal unix socket, resolves
the Lua service handles from the ROM stub's GATT database dump, injects
CONNECT/pairing when a client attaches, enables the TX/Video/AudioTX
notifications, and forwards frames both ways with end-to-end backpressure
(a stalled client stalls the firmware's notify path; nothing is dropped).

Control codes ride channel 0 into the firmware unchanged; 0x02 (reboot) makes
the guest request an SE SoC reset, which resets the machine in place — MRAM
(`/lfs`) survives, SRAM (including `__noinit`) is cleared like hardware, the
firmware boots again and the bridge re-arms for the next client.

The **control socket** (ticket 0031, `--ctl-port`, default `tcp://127.0.0.1:9562`)
drives the device the way a tester drives hardware — one text verb per line,
reply `ok [data]` / `err <reason>` (`nc 127.0.0.1 9562` for interactive use):
`button down|up|click|hold <ms>` (the LPGPIO-1 button; 150 ms click → Lua
`frame.button.single` ~400 ms after release, ≥1 s hold → `frame.button.long`;
2/5/15 s holds hit the C-hardwired deep-sleep/pairing/ship-mode paths),
`battery set 82%|4000mv|<raw>` and `battery?` (the firmware polls VBAT every
10 s — allow one poll; `frame.battery_voltage()` is unfiltered,
`frame.battery_level()` is EMA-filtered and lags), `charger on|off|?`
(STAT gpio1.3; the vbat driver re-samples on the edge, so
`frame.battery_charging()` follows immediately; `charger?` also reports the
firmware's gpio0.6 charge-control output), `led?` (UTIMER3 PWM duty/period/
enable), `reboot` (machine reset, MRAM preserved) and `wdt-fire` (inject a
watchdog timeout: NMI → the firmware's NMI handler stores its `__noinit`
magic → 200 ms later a TCM-preserving warm reset, after which the firmware's
`halo_watchdog_has_fired()` cold-boot path reboots once more via the SE).
In the QEMU window the `B` key presses/releases the button. Under QMP the
same controls are `qom-get`/`qom-set` on `/machine` (`button-pressed`,
`charger-connected`, `charge-enabled`, `battery-raw`, `led-duty`,
`led-period`, `led-on`, `wdt-fire`).

**Audio** (ticket 0032) works in both directions. The speaker is I2S0 driven by the
interrupt/FIFO path (the board's `i2s0` node has no `dmas`, so there is no DMA to model
there); samples go to `--wav-out FILE` (16-bit mono, written at a fixed 32 kHz with the
guest's 8/16 kHz clips upsampled into it, and valid on disk while the emulator runs)
and/or to the host with `--audio [BACKEND]`. The microphone is the LPPDM drained by
dma2 — QEMU's PL330 at `0x400C0000`, instantiated because the firmware's PDM driver has
no non-DMA fallback — and reads from `--wav-in FILE` (looped, resampled), from a built-in
tone, or from the host when `--audio` is on; the default is silence. The control socket
adds `mic wav <file>` / `mic tone <hz> [amp]` / `mic silence` / `mic?` and `speaker?`
(`enabled` is the MAX98357A SD_MODE on gpio8.5). Under QMP the same things are
`mic-wav-in`, `mic-tone-hz`, `mic-tone-amplitude`, `mic-source`, `mic-samples`,
`speaker-enabled`, `speaker-playing`, `speaker-rate` and `speaker-samples` on `/machine`.
Note when injecting a test signal: use a *tone*, not DC — the LPPDM runs with its IIR/FIR
bypassed but `lua_microphone.c` applies its own DC blocker, so a constant level correctly
arrives as zeros.

The ROM's **LC3 codec** is real, not a stub: `rom-stub/src/stub_lc3.c` backs the ten
pinned `lc3_api_*` entry points with Google's liblc3 (pinned + fetched by `init.sh`,
cross-compiled into the ROM image above `0x00160000`), so `frame.speaker.start{encoder=
'lc3'}` decodes stock third-party `.lc3` bitstreams and `frame.microphone.start{encoder=
'lc3'}` produces interoperable ones. The Alif ABI's opaque structs are too small for
liblc3's contexts, so the codec state lives in the caller-allocated buffers whose sizes
the ABI asks *us* for (`encoder_scratch_size` / `decoder_status_size`) — no pool, no
allocator, nothing to survive `ble_stack_init()` zeroing the ROM data region.

Tooling: `tools/repl_smoke.py` (end-to-end gate against a real image),
`tests/smoke_controls.py` (control-socket gate: button → Lua callbacks,
battery/charger, wdt-fire), `tests/smoke_audio.py` (audio gate: startup sound into the
WAV, injected tone out of `frame.microphone.read()`, PCM playback, LC3 both ways),
`tools/run_emu_tests.py` (runs the unmodified
device test-suite from a firmware checkout via `pyshim/brilliant_ble`, the
phone-library shim; exports `HALO_EMU_CTL` so tests can reach the control
socket through `brilliant_ble.emu_control()`).
Note: a firmware built without git metadata has an empty `frame.GIT_TAG`, and
tests that `print(frame.GIT_TAG)` with `await_print` hang on any transport
(hardware included) — `print("")` emits nothing. Build the image from a git
checkout (`west build -b halo --sysbuild alif/applications/halo`) for the
full device-test subset.

Since ticket 0028 the synthetic BLE ROM stub (`rom-stub/`, loaded automatically by
`halo-emu`) unblocks `main()` and can expose the raw GATT **doorbell bridge** on
TCP instead (`--ble-port 9564`, which turns the REPL bridge off): framed
`{u8 op, u8 flags, u16 len, payload}` messages to connect/pair, inject GATT
writes and collect notifications (contract: `rom-stub/src/halo_rom_ipc.h`).
Ticket 0030's REPL rides on it.

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
133: vga020 `0x54`, TPS65132 `0x3E`, PAG7982 camera `0x40`. **Audio out** (modeled,
0032): `alif,i2s-sync` `0x49014000`/IRQ 141 (ISR in DTCM) + MAX98357A (gpio8.5); 16-deep
TX FIFO, trigger 8, `ISR.TXFE` is a level and the driver's ISR refills until the FIFO is
above it — but TXFE must stay low until the first sample clocks out, or the level IRQ
raised inside `i2s_send()` re-enters before `tx.running` is set and boot deadlocks.
**Mic** (modeled, 0032): LPPDM `0x43002000`/IRQ 49, DMA PL330 `0x400C0000` (IRQs 0-4,32)
ch4, request line 30; the read port at `CH2_CH3_AUDIO_OUT` is width-aware because
Zephyr's PL330 driver reads it in two 16-bit beats at a fixed address per 32-bit set.
Camera: LPCAM `0x43003000`/IRQ 54 (parallel
8-bit). GPIO: DW banks gpio0–9 `0x49000000..0x49009000` (IRQs 179–253), LPGPIO
`0x42002000` (IRQs 171/172) — **button = LPGPIO pin 1, active-low**. Pinctrl `0x1A603000`/
`0x42007000` (direct writes, no SE). BLE sync timer UTIMER0 `0x48001000` + EVTRTR
`0x400E2000` (IRQs 377/384). Disabled in DT (not needed): all other UARTs, SPI, OSPI/XIP,
SD, CAN, I3C, DAC, second ADC.
