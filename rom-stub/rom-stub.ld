/*
 * rom-stub.ld — placement of the synthetic BLE/LC3 ROM stub inside the
 * halo machine's ROM window (0x00090000..0x00160000).  See src/stub.h for
 * the layout rationale and src/halo_rom_ipc.h for the shared-ring / MMIO
 * contract with the QEMU doorbell device.
 */

MEMORY
{
    HDR    (r)  : ORIGIN = 0x00090000, LENGTH = 0x100
    VENEER (rx) : ORIGIN = 0x0009F094, LENGTH = 0x00140D58 - 0x0009F094
    TEXT   (rx) : ORIGIN = 0x00141000, LENGTH = 0x0000D000
    DATA   (rw) : ORIGIN = 0x0014E000, LENGTH = 0x00008000
    /* rings at 0x156000/0x15A000 are shared with the QEMU device and not
     * touched by the linker */
}

ENTRY(rom_stub_trap_entry)

SECTIONS
{
    .romhdr : { KEEP(*(.romhdr)) } > HDR

    .romvec : { KEEP(*(.romvec)) } > VENEER

    .text :
    {
        *(.text.traps)
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > TEXT

    /* No crt0: ble_stack_init() zeroes the whole DATA region, so only
     * zero-initialized state is allowed here. */
    .data : { *(.data*) } > DATA
    ASSERT(SIZEOF(.data) == 0, "rom-stub: initialized .data is not supported (no loader)")

    .bss (NOLOAD) :
    {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > DATA

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
