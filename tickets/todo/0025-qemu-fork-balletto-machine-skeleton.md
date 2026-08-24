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
