/*
 * fw_dispsmoke.c — bare-metal test firmware for the display path
 * (ticket 0029).
 *
 * There is still no firmware build (zephyr.bin) on this host, so the
 * gate is exercised with this image: it re-enacts the hardware
 * sequence the halo firmware's display bring-up performs, at the
 * register level the QEMU models implement:
 *
 *   1. TPS65132 PMIC @ I2C1 0x3E: read VPOS, program it, read back
 *      (regulator_tps65132.c pattern)              => "pmic-ok"
 *   2. D-PHY start-up: poll DSI_PHY_STATUS for PHY_LOCK, then the
 *      stop-state bits (dphy_dw.c bounded polls)    => "dsi-ok"
 *   3. vga020 panel @ I2C1 0x54: 16-bit reg write 0x6C00=0x00
 *      (vga020_hw_init pattern)                     => "panel-ok"
 *   4. CDC200: program the 256x256 timings, layer 1 RGB888 pointed at
 *      the DTCM framebuffer's 0x58930000 global alias (fb0_0's real
 *      address), draw 8 vertical color bars, unmask the LINE irq,
 *      enable                                       => "display-on"
 *   5. count scanline IRQs (IRQ 333); after 3        => "scanline-ok"
 *      then                                          => "ready"
 *      — the harness screendumps here and checks the bars
 *   6. hold ~3 s (90 more scanlines, the splash pattern), disable the
 *      controller                                    => "display-off"
 *      — the harness screendumps again and checks a blanked panel.
 *
 * Console: UART3 (DW 16550, reg-shift 2) @ 0x4901B000.
 */

#include <stdint.h>

#define BARRIER() __asm__ volatile("" ::: "memory")

static inline void reg_write(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t reg_read(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* ------------------------------------------------------------------ */
/* UART3 console (DW 16550, reg-shift 2)                               */
/* ------------------------------------------------------------------ */

#define UART3_BASE 0x4901B000u
#define UART_THR   (UART3_BASE + (0 << 2))
#define UART_LSR   (UART3_BASE + (5 << 2))
#define LSR_THRE   (1u << 5)

static void puts_uart(const char *s)
{
    while (*s) {
        while (!(reg_read(UART_LSR) & LSR_THRE)) {
        }
        reg_write(UART_THR, *s++);
    }
    while (!(reg_read(UART_LSR) & LSR_THRE)) {
    }
    reg_write(UART_THR, '\n');
}

/* ------------------------------------------------------------------ */
/* I2C1 (DesignWare) master helpers                                    */
/* ------------------------------------------------------------------ */

#define I2C1_BASE      0x49011000u
#define IC_CON         (I2C1_BASE + 0x00)
#define IC_TAR         (I2C1_BASE + 0x04)
#define IC_DATA_CMD    (I2C1_BASE + 0x10)
#define IC_RAW_INTR    (I2C1_BASE + 0x34)
#define IC_CLR_TX_ABRT (I2C1_BASE + 0x54)
#define IC_ENABLE      (I2C1_BASE + 0x6C)
#define IC_STATUS      (I2C1_BASE + 0x70)
#define IC_RXFLR       (I2C1_BASE + 0x78)

#define CON_MASTER      0x01u
#define CON_SPEED_STD   0x02u
#define CON_RESTART_EN  0x20u
#define CON_SLAVE_DIS   0x40u
#define CMD_READ        (1u << 8)
#define CMD_STOP        (1u << 9)
#define CMD_RESTART     (1u << 10)
#define STATUS_TFE      (1u << 2)
#define STATUS_RFNE     (1u << 3)
#define INTR_TX_ABRT    (1u << 6)

static void i2c1_target(uint8_t addr)
{
    reg_write(IC_ENABLE, 0);
    reg_write(IC_CON, CON_MASTER | CON_SPEED_STD | CON_RESTART_EN |
                      CON_SLAVE_DIS);
    reg_write(IC_TAR, addr);
    reg_write(IC_ENABLE, 1);
}

static int i2c1_ok(void)
{
    for (int i = 0; i < 100000 && !(reg_read(IC_STATUS) & STATUS_TFE); i++) {
    }
    if (reg_read(IC_RAW_INTR) & INTR_TX_ABRT) {
        (void)reg_read(IC_CLR_TX_ABRT);
        return 0;
    }
    return 1;
}

static int i2c1_write(uint8_t addr, const uint8_t *buf, int len)
{
    i2c1_target(addr);
    for (int i = 0; i < len; i++) {
        reg_write(IC_DATA_CMD, buf[i] | (i == len - 1 ? CMD_STOP : 0));
    }
    return i2c1_ok();
}

static int i2c1_write_read(uint8_t addr, const uint8_t *wbuf, int wlen,
                           uint8_t *rbuf, int rlen)
{
    i2c1_target(addr);
    for (int i = 0; i < wlen; i++) {
        reg_write(IC_DATA_CMD, wbuf[i]);
    }
    for (int i = 0; i < rlen; i++) {
        /* repeated-start on the write->read direction change */
        reg_write(IC_DATA_CMD, CMD_READ | (i == 0 ? CMD_RESTART : 0) |
                               (i == rlen - 1 ? CMD_STOP : 0));
        for (int j = 0; j < 100000 && !reg_read(IC_RXFLR); j++) {
        }
        if (!reg_read(IC_RXFLR)) {
            return 0;
        }
        rbuf[i] = reg_read(IC_DATA_CMD) & 0xff;
    }
    return i2c1_ok();
}

/* ------------------------------------------------------------------ */
/* DSI host: the dphy_dw.c poll targets                                */
/* ------------------------------------------------------------------ */

#define DSI_BASE       0x49032000u
#define DSI_PHY_RSTZ   (DSI_BASE + 0xA0)
#define DSI_PHY_STATUS (DSI_BASE + 0xB0)

#define PHY_LOCK        (1u << 0)
#define STOPSTATE_BITS  ((1u << 2) | (1u << 4) | (1u << 7))

static int dsi_phy_up(void)
{
    reg_write(DSI_PHY_RSTZ, 0x7); /* enableclk | shutdownz | rstz */
    for (int i = 0; i < 1000; i++) {
        if ((reg_read(DSI_PHY_STATUS) & PHY_LOCK) == PHY_LOCK) {
            break;
        }
    }
    if (!(reg_read(DSI_PHY_STATUS) & PHY_LOCK)) {
        return 0;
    }
    return (reg_read(DSI_PHY_STATUS) & STOPSTATE_BITS) == STOPSTATE_BITS;
}

/* ------------------------------------------------------------------ */
/* CDC200 @ 256x256 RGB888 — the halo panel configuration              */
/* ------------------------------------------------------------------ */

#define CDC_BASE       0x49031000u
#define CDC_SYNC_SIZE  (CDC_BASE + 0x08)
#define CDC_BP         (CDC_BASE + 0x0C)
#define CDC_ACTW       (CDC_BASE + 0x10)
#define CDC_TOTALW     (CDC_BASE + 0x14)
#define CDC_GLB_CTRL   (CDC_BASE + 0x18)
#define CDC_BG_COLOR   (CDC_BASE + 0x2C)
#define CDC_IRQ_MASK0  (CDC_BASE + 0x34)
#define CDC_IRQ_STATUS0 (CDC_BASE + 0x38)
#define CDC_IRQ_CLEAR0 (CDC_BASE + 0x3C)
#define CDC_LINE_POS   (CDC_BASE + 0x40)
#define CDC_L1_CTRL    (CDC_BASE + 0x10C)
#define CDC_L1_HPOS    (CDC_BASE + 0x110)
#define CDC_L1_VPOS    (CDC_BASE + 0x114)
#define CDC_L1_PIXFMT  (CDC_BASE + 0x11C)
#define CDC_L1_ALPHA   (CDC_BASE + 0x120)
#define CDC_L1_CFB_ADDR   (CDC_BASE + 0x134)
#define CDC_L1_CFB_LENGTH (CDC_BASE + 0x138)
#define CDC_L1_CFB_LINES  (CDC_BASE + 0x13C)

/* halo panel: 256x256, hsync 96 hbp 48, vsync 3 vbp 26 (halo.dts) */
#define W       256
#define H       256
#define HSW     96
#define HBP     48
#define HFP     16
#define VSW     3
#define VBP     26
#define VFP     16

#define HBP_ACC (HSW - 1 + HBP)  /* 143 */
#define VBP_ACC (VSW - 1 + VBP)  /* 28 */

/*
 * Framebuffer at the real fb0_0 address (CPU 0x20130000), programmed
 * through its 0x58930000 global alias like the real driver does.
 */
#define FB_CPU    0x20130000u
#define FB_GLOBAL 0x58930000u
#define FB_PITCH  (W * 3)

static const uint8_t bar_rgb[8][3] = {
    { 0xff, 0xff, 0xff }, { 0xff, 0xff, 0x00 }, { 0x00, 0xff, 0xff },
    { 0x00, 0xff, 0x00 }, { 0xff, 0x00, 0xff }, { 0xff, 0x00, 0x00 },
    { 0x00, 0x00, 0xff }, { 0x20, 0x20, 0x20 },
};

static void fill_bars(void)
{
    volatile uint8_t *fb = (volatile uint8_t *)FB_CPU;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *c = bar_rgb[x / (W / 8)];
            volatile uint8_t *p = fb + y * FB_PITCH + x * 3;

            /* CDC200 RGB888 memory order: B, G, R */
            p[0] = c[2];
            p[1] = c[1];
            p[2] = c[0];
        }
    }
    BARRIER();
}

static void cdc200_setup(void)
{
    reg_write(CDC_SYNC_SIZE, ((HSW - 1) << 16) | (VSW - 1));
    reg_write(CDC_BP, (HBP_ACC << 16) | VBP_ACC);
    reg_write(CDC_ACTW, ((HBP_ACC + W) << 16) | (VBP_ACC + H));
    reg_write(CDC_TOTALW, ((HBP_ACC + W + HFP) << 16) | (VBP_ACC + H + VFP));
    reg_write(CDC_BG_COLOR, 0x000000);
    reg_write(CDC_LINE_POS, VSW + VBP + H);

    reg_write(CDC_L1_PIXFMT, 1); /* RGB888 */
    reg_write(CDC_L1_CFB_ADDR, FB_GLOBAL);
    reg_write(CDC_L1_CFB_LENGTH, (FB_PITCH << 16) | (FB_PITCH + 7));
    reg_write(CDC_L1_CFB_LINES, H);
    reg_write(CDC_L1_HPOS, ((HBP_ACC + W) << 16) | (HBP_ACC + 1));
    reg_write(CDC_L1_VPOS, ((VBP_ACC + H) << 16) | (VBP_ACC + 1));
    reg_write(CDC_L1_ALPHA, 0xff);
    reg_write(CDC_L1_CTRL, 1); /* layer enable */

    reg_write(CDC_IRQ_MASK0, 1); /* LINE */
    reg_write(CDC_GLB_CTRL, reg_read(CDC_GLB_CTRL) | 1);
}

/* ------------------------------------------------------------------ */
/* Scanline IRQ (333)                                                  */
/* ------------------------------------------------------------------ */

static volatile uint32_t scanlines;

void cdc_scanline_irq_handler(void)
{
    uint32_t st = reg_read(CDC_IRQ_STATUS0);

    reg_write(CDC_IRQ_CLEAR0, st);
    scanlines++;
}

#define NVIC_ISER ((volatile uint32_t *)0xE000E100u)
#define CDC_SCANLINE_IRQ 333

static void nvic_enable(int irq)
{
    NVIC_ISER[irq / 32] = 1u << (irq % 32);
}

/* ------------------------------------------------------------------ */

void tfw_main(void)
{
    uint8_t vsel;

    puts_uart("dispsmoke: boot");

    /* 1. PMIC: program VPOS (reg 0x00), read back */
    {
        static const uint8_t wr[] = { 0x00, 0x0f };
        uint8_t reg = 0x00;

        if (i2c1_write(0x3E, wr, 2) &&
            i2c1_write_read(0x3E, &reg, 1, &vsel, 1) && vsel == 0x0f) {
            puts_uart("pmic-ok");
        } else {
            puts_uart("pmic-FAIL");
        }
    }

    /* 2. D-PHY lock + stop-state */
    puts_uart(dsi_phy_up() ? "dsi-ok" : "dsi-FAIL");

    /* 3. vga020: 16-bit reg 0x6C00 = 0x00 */
    {
        static const uint8_t wr[] = { 0x6C, 0x00, 0x00 };

        puts_uart(i2c1_write(0x54, wr, 3) ? "panel-ok" : "panel-FAIL");
    }

    /* 4. framebuffer + CDC200 up */
    fill_bars();
    nvic_enable(CDC_SCANLINE_IRQ);
    cdc200_setup();
    puts_uart("display-on");

    /* 5. wait for scanline interrupts */
    while (scanlines < 3) {
        __asm__ volatile("wfi");
    }
    puts_uart("scanline-ok");
    puts_uart("ready");

    /*
     * 6. hold the frame ~3 s (like the boot-logo splash), then disable —
     * the window must blank (GLB_CTRL bit0 is the on/off switch).
     */
    while (scanlines < 3 + 90) {
        __asm__ volatile("wfi");
    }
    reg_write(CDC_GLB_CTRL, reg_read(CDC_GLB_CTRL) & ~1u);
    puts_uart("display-off");

    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ------------------------------------------------------------------ */
/* Vector table / reset (self-contained: the scanline vector differs   */
/* from fw_start.c's BLE image)                                        */
/* ------------------------------------------------------------------ */

extern uint32_t __bss_start__[];
extern uint32_t __bss_end__[];
extern uint32_t __stack_top__[];

#define NUM_EXCEPTIONS (16 + 480)

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

static void default_handler(void)
{
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
    [16 + CDC_SCANLINE_IRQ] = cdc_scanline_irq_handler,
};
