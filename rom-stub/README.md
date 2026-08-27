# Synthetic BLE/LC3 ROM stub (ticket 0028)

The halo firmware's BLE host stack lives in the Balletto B1's on-chip ROM:
the application links against **993 absolute symbols** (983 BLE + 10 LC3,
symbol map **v1_2**) and `BL`s straight into them.  The real ROM cannot be
dumped (no device/SWD access), so this directory builds our own replacement:
bare-metal C linked at exactly those addresses, implementing only the API
surface the firmware actually uses.

## What is implemented

Derived by intersecting the symbol maps with the identifiers referenced by
the firmware sources (`modules/halo/src/ble_*.c`, `modules/hal/alif/ble/plf/`)
— 80 entry points, two tiers:

- **Semantic** — GAPM configuration + advertising chain, single-connection
  GAPC with a deterministic Just-Works pairing emulation, GATT server
  (user/db registration, write/read/notify data path), minimal GATT client
  (ANCS discovery completes "not found"), `co_buf` pool, `co_rand_word`.
  Procedures complete synchronously (completion callbacks run inside the
  call — the firmware's counting semaphores tolerate that).
  Since 0032/0038/0039 this also covers the whole audio surface: the LC3
  codec (`stub_lc3.c`, liblc3), the GAF profile layer — BAP unicast and
  capabilities servers, CAP, TMAP, ARC (`stub_gaf.c`) — the ASE state
  machine (`stub_ase.c`) and the isochronous data path (`stub_iso.c`).
  The "declared-unsupported" tier this file used to describe is gone, and
  `stub_misc.c` with it.
- **Trap thunks** — every other pinned symbol (913) branches to a thunk
  that reports symbol index + caller LR to the QEMU doorbell device (logged
  by name via the generated `.syms` table) and returns
  `GAP_ERR_NOT_SUPPORTED`: a missed dependency is loud, never silent.

## Transport (doorbell device)

GATT traffic is bridged to the host through `hw/arm/halo_ble.c` in the QEMU
fork: two shared byte rings in the ROM window + a doorbell MMIO page
(contract: `src/halo_rom_ipc.h`, mirrored in `patches/files/hw/arm/`).
Host→guest frames are signalled over NVIC IRQ 377 (UTIMER0 capture A — the
IRQ the firmware's BLE sync-timer driver already connects); the stub
registers its capture callback through the app hook table and drains the
ring from `rwip_process()` on the BLE task.  `halo-emu` exposes the bridge
as `tcp://127.0.0.1:9564` (`--ble-port`); ticket 0030 builds the Lua REPL
bridge on it.

## Layout

```
gen_rom_layout.py   .lds maps + nm output -> veneers.S (4-byte b.w per
                    pinned address) + rom-stub-v1_2.syms
rom-stub.ld         window layout: header @0x90000, veneers @0x9F094,
                    .text @0x141000, .bss @0x14E000, rings @0x156000+
src/                stub implementation (hstub_<symbol> = implementation
                    of pinned <symbol>; add one and the veneer generator
                    picks it up automatically)
vendor/             Alif BLE v1_2 / LC3 v1_2 headers + symbol maps,
                    vendored verbatim (declarations only, Apache-2.0)
shim/               freestanding arch.h/compiler.h (replaces the
                    Zephyr-dependent plf headers)
test/               bare-metal gate firmware (see below)
```

## Build & test

```sh
make                # rom-stub-v1_2.{bin,syms,elf}  (halo-emu picks them up)
make test-fw        # test/fw_blesmoke.c -> build/fw_blesmoke.bin
python3 ../tests/smoke_ble.py
```

Toolchain: `arm-none-eabi-gcc` (system, or `../deps/toolchain` fetched by
`../init.sh`).

The stub is pinned to ROM symbol map **v1_2**
(`CONFIG_ALIF_BLE_ROM_IMAGE_V1_2`); a firmware built against another map
needs a matching rebuild — mismatches show up as trap reports at bogus
symbols right after boot.

## Debugging

`halo-emu --gdb`, then:

```
target remote :1234
add-symbol-file rom-stub/build/rom-stub-v1_2.elf
```
