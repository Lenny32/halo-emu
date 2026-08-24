/*
 * fw.ld — bare-metal test firmware layout: app slot0 image (load base
 * 0x80020000, vector table at +0x800 like a real zephyr.bin), .bss and
 * stack in DTCM.  ROM symbols come from the rom-stub ELF (--just-symbols).
 */

MEMORY
{
    APP  (rx) : ORIGIN = 0x80020800, LENGTH = 0x40000
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 0x00100000
}

ENTRY(tfw_main)

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > APP

    .data : { *(.data*) } > APP
    ASSERT(SIZEOF(.data) == 0, "test fw: initialized .data is not supported")

    .bss (NOLOAD) :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > DTCM

    __stack_top__ = ORIGIN(DTCM) + LENGTH(DTCM);

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
