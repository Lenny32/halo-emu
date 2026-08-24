# 0025 — QEMU fork + Alif Balletto B1 machine skeleton

**Phase:** 2 — QEMU machine emulation (tickets 0001–0024 retired, see git tag `archive/native-sim`)
**Depends on:** —
**Effort:** L

## Story

The emulator executes the real device firmware binary: `qemu-system-arm -M halo -f zephyr.bin`.
This ticket vendors QEMU, adds the `halo` machine (Alif **Balletto B1**, Cortex-M55 RTSS-HE)
with the correct memory map and console UART, and boots the unmodified app image far enough
to prove instruction-level execution works. No SE/BLE yet — the expected end-state is the
firmware busy-spinning on the Secure Enclave mailbox (ticket 0026 unblocks that).

Hard rule unchanged: everything stays inside `emulator/`. The firmware tree
`~/halo-firmware` is a read-only *reference* during development; the emulator artifact
depends only on the `-f` binary at runtime.

## Tasks

1. `init.sh`: shallow-clone a pinned QEMU stable release into `emulator/qemu/` (git,
   `--depth 1 --branch <pin>`; pick the newest stable at implementation time, >= 8.2 which
   already has Cortex-M55 + MVE via `mps3-an547`), apt deps (ninja, glib, pixman, SDL2),
   `configure --target-list=arm-softmmu`, build. Idempotent re-runs.
2. Machine `hw/arm/halo.c` (new file in the fork, kept as a patch/commit on our branch):
   - CPU `cortex-m55`, `CONFIG_NUM_IRQS=480` external IRQs, 8 NVIC priority bits.
   - Memory map (from `~/halo-firmware/build/halo/zephyr/zephyr.map` "Memory Configuration"):

     | Region | Base | Size | Notes |
     |---|---|---|---|
     | MRAM (flash) | `0x80000000` | 2 MB | plain RAM for now (persistence = ticket 0027); *writable* — littlefs + mcumgr write it |
     | ITCM | `0x00000000` | 512 KB | alias also at `0x58000000` |
     | ROM window | `0x00090000–0x00160000` | — | placeholder RAM region for the synthetic BLE/LC3 ROM (ticket 0028); overlaps upper ITCM addressing — map as one region above ITCM if simpler |
     | DTCM | `0x20000000` | 1.5 MB (`0x20000000–0x20180000`, incl. `.alif_ns` @ `0x200E0000`) | alias at global `0x58800000 + offset` (`LOCAL_TO_GLOBAL(x) = x - 0x20000000 + 0x58800000`) |
   - Aliases matter: SE messages and the CDC200 framebuffer are addressed via `0x58...`.
   - Background: map the whole peripheral space as `unimplemented-device` (read 0 / ignore
     writes, `-d unimp` traces) so unknown MMIO never faults. Boot-time raw pokes that must
     be tolerated: `0x1A60xxxx` (CGU/AON/VBAT), `0x4902F008`, `0x43007000..10`, `0x400E2080`,
     `0x4300A00C`/`0x4300B00C`.
   - TGU: ITGU `0xE001E500`, DTGU `0xE001E600` — implement readable `CFG` (offset +4) with a
     sane BLKSZ and writable LUTs (SystemInit's `sau_tcm_ns_setup()` loops over them).
     Note the `0xE001Exxx` range sits in the PPB — needs explicit handling next to the NVIC.
   - SAU: the app runs Secure and programs one NS region (`0x200E0000–0x2017FFFF`); QEMU's
     M-profile security support handles this — enable the security extension on the CPU.
3. Loader: `-device loader` semantics wrapped by the machine: accept the raw app image
   placed at `0x80020000`; initial vectors live at `0x80020800` (`CONFIG_ROM_START_OFFSET=0x800`
   pad; `zephyr.signed.bin` has its imgtool header in that pad and loads identically).
   Set the machine's `init-svtor`/vector base so reset fetches MSP=`0x200D4678`-style values
   from `0x80020800` (do not hardcode — read from the loaded image).
4. Console UART3: `ns16550` @ `0x4901B000`, `reg-shift=2`, IRQ **127**, 115200, ref clock
   40 MHz — instantiate QEMU `serial-mm`, plus tolerance for the DesignWare **DLF register at
   offset `0xC0`** (cover with a tiny wrapper or an adjacent unimplemented region so the
   driver's `dlf` write doesn't fault). Wire to stdio chardev. (No output expected this
   ticket — console driver init happens after the SE spin — but the device must exist.)
5. SysTick is built into QEMU's armv7m at `0xE000E010`; ensure
   `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=160000000` maps to a sensible systick scale.

## Key points in code

- Reference build artifacts (read-only): `~/halo-firmware/build/halo/zephyr/{.config,zephyr.dts,zephyr.map}`
- Boot entry chain to satisfy: `z_arm_reset` → `SystemInit`
  (`~/halo-firmware/modules/hal/alif/common/src/system.c:44`): FAULTMASK, MPU disable,
  SHCSR fault enables, CPACR CP10/11, cache enables, then `sau_tcm_ns_setup()`
  (`common/src/sau_tcm_ns_setup.c` — SAU region + TGU LUT loops).
- SE spin location (this ticket's end state): `se_service_sync_locked()` polling loops in
  `~/halo-firmware/modules/hal/alif/se_services/zephyr/src/se_service.c` (SYNC_TIMEOUT 500 ms
  × MAX_TRIES 100, cycle-counted busy loops pre-kernel).

## Gate (acceptance)

- `init.sh` produces `qemu/build/qemu-system-arm` reproducibly.
- Running the real `~/halo-firmware/build/halo/zephyr/zephyr.bin`:
  no CPU exception/abort through `SystemInit` and kernel early init; execution demonstrably
  reaches the MHUv2/SE busy-wait (shown via `-d unimp` trace hitting `0x40050000` region, or
  gdbstub backtrace into `se_service.c` addresses from the map).
- `-d unimp` log reviewed: no *faulting* accesses, only tolerated unimplemented ones.

## Implementation notes (2026-08-24, done)

- **Pin:** QEMU `v11.1.0` (newest stable), shallow-cloned by `init.sh` into `qemu/`;
  the machine lives in `patches/files/hw/arm/halo.c` + `patches/qemu-build-integration.patch`
  (hw/arm/Kconfig `config HALO`, hw/arm/meson.build), overlaid and committed by `init.sh`
  onto fork branch `halo`.
- **pixman fallback:** apt install is attempted but this host has no passwordless sudo, so
  `init.sh` builds a pinned static pixman (`pixman-0.44.2`, meson from a pip venv) into
  `deps/` when the system package is missing. glib/ninja/SDL2 were already present.
- **TGU:** as the ticket warned, `0xE001Exxx` cannot be mapped in system memory — the
  armv7m container covers the whole System PPB with a RAZ/WI default region that shadows
  board memory. The ITGU/DTGU io regions are mapped into `armv7m.container` at
  systick-style priority 1. CFG reads BLKSZ=7 (4 KiB blocks); LUTs are plain R/W state.
  Verified via monitor: after boot DTGU LUT7..11 = 0xFFFFFFFF (NS region
  0x200E0000–0x2017FFFF = blocks 224–383), ITGU CFG reads 0x7.
- **UART3:** QEMU `serial-mm` @ 0x4901B000 (reg-shift 2, IRQ 127, baudbase 40 MHz/16);
  the DesignWare DLF write (offset 0xC0, dt `dlf = <0xb>`) falls through to the
  background `halo.periph` unimplemented region — read-0/write-ignore, driver tolerates.
- **ROM window:** mapped as RAM 0x00090000..0x00160000 with a global alias at 0x58090000
  (no ITCM overlap: ITCM ends at 0x80000).
- **Gate result** with real `zephyr.bin` (0.8.8 @ d1a9645, MSP 0x200D4678 / PC z_arm_reset
  fetched from 0x80020800 via `init-svtor`):
  - Boots through `SystemInit` (incl. `sau_tcm_ns_setup()`) and full kernel init with
    **zero** faulting accesses; `-d unimp,guest_errors` shows only tolerated RAZ/WI
    (NVIC ACTLR offset 0x8 ×2) and unimplemented-device traces.
  - Reaches the MHUv2/SE mailbox: 304 accesses in the 0x40040F88–F98 sender frame —
    `se_service_sync_locked()`'s 100 tries — with the console (already live!) printing
    101× `<err> se_service: failed to send service 0`, then the documented no-SE end
    state: `Failed to synchronize with SE (errno=-22)` → fatal assert path.
  - Execution then continues into later device init and dies in the firmware's own
    display D-PHY code (division by zero on an unimplemented 0x4903F000 clock read) —
    beyond this ticket's scope; ticket 0026's SE fake + stubs own that path.
- Deliberately deferred: MRAM persistence (0027), any MHU response (0026), TGU LUTs are
  state-only (no actual gating), no machine reset handler for TGU state (reboot re-ORs
  the same bits, harmless until a ticket needs clean reset).
