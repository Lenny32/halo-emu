# Halo desktop emulator — QEMU machine emulation

Goal: **run the real device firmware binary**, unmodified —

```sh
./init.sh                     # once: fetch + build the QEMU fork and the ROM stub
./halo-emu --fetch latest     # download the newest firmware release and run it
./halo-emu -f zephyr.bin      # or a firmware you built in ~/halo-firmware
```

— with the 256×256 UI in a window, `/lfs` persisted to a host file, and the Lua REPL on
`tcp://127.0.0.1:9563`.

`--fetch <tag|latest>` (ticket 0034) pulls a release of
`brilliantlabsAR/halo-firmware` into `firmwares/<tag>/` and boots it, so a clean clone
needs neither a firmware checkout nor a firmware build toolchain.
`.github/workflows/ci.yml` does exactly that on every push and nightly: build the QEMU
fork (cached), boot the release headless, run the REPL smoke and the M1 device-test
subset. See `EMULATOR.md` for the details.

## Architecture decision (2026-08-24)

The first emulator generation (tickets 0001–0008) compiled the firmware **sources** for
Zephyr `native_sim` — same code, but a host build, so it could never execute a firmware
*artifact*. The user's requirement is artifact-level testing, so that generation is
retired; everything about it is recoverable from the git tag **`archive/native-sim`**.

This generation is a **QEMU machine model** of the Halo's SoC — the Alif **Balletto B1**
(Cortex-M55 RTSS-HE) — built as a patched QEMU fork (machine `halo`). The firmware binary
executes instruction-for-instruction; the hardware around it is modeled. Full hardware
reference (memory map, peripherals, boot-fatal dependencies): `EMULATOR.md`.

Key strategy points:

- **Secure Enclave**: faked at the MHUv2 mailbox (`0x40040000`/`0x40050000`) with an
  ack-and-zero responder — sufficient for every boot-path call.
- **LC3**: the same on-chip ROM holds the LC3 codec, and the firmware exchanges real LC3
  bitstreams with the phone, so its ten pinned entry points are backed by Google's
  **liblc3** (fetched by `init.sh`, cross-compiled into the ROM stub) rather than stubbed.
- **BLE**: the firmware's BLE host stack lives in on-chip ROM and is called through 983
  pinned addresses. No device/SWD access exists to dump that ROM, so the **permanent**
  strategy is a **synthetic ROM stub** — our own code linked at those addresses,
  implementing just the GAPM/GATT surface the app uses, bridging the Lua REPL out to TCP.
  No proprietary code involved. Consequence: real radio behavior is out of scope; the
  stub is pinned to ROM symbol map v1_2 and must track firmware ROM-map bumps.
- **Display**: the CDC200 controller continuously scans out a fixed RGB888 buffer —
  modeled as a QEMU display device (the UI window).
- **Independence**: the emulator's only runtime input is the `-f` binary (plus optional
  MRAM image). `~/halo-firmware` is used read-only as a development *reference*; no
  build-time coupling to any firmware checkout.

## Ticket queue

| # | Ticket | Delivers |
|---|--------|----------|
| 0025 | qemu-fork-balletto-machine-skeleton | QEMU fork builds; real zephyr.bin executes to the SE spin |
| 0026 | se-mhuv2-fake-and-boot-critical-stubs | boot banner + logs, littlefs mounts, parks at BLE init |
| 0027 | halo-emu-launcher-and-mram-persistence | `halo-emu -f firmware.bin`, persistent `/lfs` |
| 0028 | synthetic-ble-rom-stub | `main()` completes; Lua runtime starts — **risk item** |
| 0029 | cdc200-display-window | boot logo + live UI in the QEMU window |
| 0030 | lua-repl-bridge | REPL on TCP 9563; smoke + device tests rewritten |
| 0031 | inputs-and-controls | button, battery, LED readout, reboot/wdt hooks |
| 0032 | audio-optional | speaker to WAV/host audio, mic from WAV/tone, LC3 via liblc3 |
| 0033 | camera-optional | camera from host files (optional) |
| 0034 | ci-and-firmware-fetch | `halo-emu --fetch <version>`, ROM-map guard, GitHub Actions boot/REPL/device-test smoke |

Sizing honesty: 0025–0027 are each days-to-a-week; **0028 is weeks** (the API surface of
the ROM stub is reverse-engineered from headers + the link map); 0029–0030 about a week
each. The UI first appears at 0029, the REPL at 0030. Until 0026 lands, nothing runnable
exists in this repo.

## Process

Per `AGENTS.md`: everything stays inside `emulator/`; tickets are implemented in order,
one at a time, and move from `tickets/todo/` to `tickets/done/` when their gate passes.
