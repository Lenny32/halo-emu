/*
 * fw_start.c — vector table and reset path for the bare-metal test
 * firmware.  Image layout matches a real app image: loaded at MRAM
 * 0x80020000 with the vector table at +0x800 (CONFIG_ROM_START_OFFSET pad).
 *
 * .data must stay empty (asserted in fw.ld): the image runs in place from
 * MRAM and only .bss (zeroed here) lives in DTCM.
 */

#include <stdint.h>

void tfw_main(void);
void ble_doorbell_irq_handler(void);

extern uint32_t __bss_start__[];
extern uint32_t __bss_end__[];
extern uint32_t __stack_top__[];

#define NUM_EXCEPTIONS (16 + 480)
#define BLE_IRQ 377

static void reset_handler(void)
{
    for (uint32_t *p = __bss_start__; p < __bss_end__; p++) {
        *p = 0;
    }
    __asm__ volatile("cpsie i");
    tfw_main();
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* GCC emits these even with -ffreestanding */
void *memset(void *dst, int c, unsigned n)
{
    uint8_t *d = dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

static void default_handler(void)
{
    /* Unexpected exception: park (visible via gdb) */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

__attribute__((section(".vectors"), used))
static void (*const vectors[NUM_EXCEPTIONS])(void) = {
    [0] = (void (*)(void))__stack_top__,
    [1] = reset_handler,
    [2] = default_handler,  /* NMI */
    [3] = default_handler,  /* HardFault */
    [4] = default_handler,  /* MemManage */
    [5] = default_handler,  /* BusFault */
    [6] = default_handler,  /* UsageFault */
    [11] = default_handler, /* SVCall */
    [14] = default_handler, /* PendSV */
    [15] = default_handler, /* SysTick */
    [16 + BLE_IRQ] = ble_doorbell_irq_handler,
};
