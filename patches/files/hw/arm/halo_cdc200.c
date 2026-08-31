/*
 * Halo — TES CDC200 display controller @ 0x49031000 (tes,cdc-2.1),
 * scanline_0 IRQ 333: the emulator's UI window.
 *
 * The firmware side is Zephyr's display_cdc200.c.  It continuously
 * scans out fixed framebuffers, so the model presents the buffer at
 * the programmed layer CFB address to a QEMU graphic console and
 * raises the LINE interrupt once per frame while the controller is
 * enabled — the driver's double-buffer commit (cdc200_swap_fb) runs in
 * that ISR: it rewrites Ln_CFB_ADDR and pulses SRCTRL, and because all
 * registers here take effect immediately the next scanout picks the
 * new buffer up with no extra shadow-reload machinery.
 *
 * Register contract (from display_cdc200.c/.h — only what the driver
 * touches is given behavior, everything else is plain read/write
 * storage):
 *  - GLB_CTRL bit0 (CDC_EN) is the on/off switch: set = scan out and
 *    raise LINE per frame, clear = blank window, no interrupts.  The
 *    other GLB_CTRL bits (signal polarity) are don't-cares here.
 *  - IRQ_MASK0/STATUS0/CLEAR0 bit0 = LINE.  STATUS accumulates raw
 *    events; MASK gates the NVIC line; CLEAR is write-one-to-clear.
 *  - Timings are the accumulated STM32-LTDC-style values:
 *    BP = sync+bp-1, ACTW = BP+active, so the panel size is
 *    ACTW-BP per axis (halo: 256x256) and a layer window's on-screen
 *    origin is WIN_xPOS.start - BP - 1.
 *  - Layer n block at 0x100*n: CTRL bit0 enables the layer,
 *    PIX_FORMAT selects ARGB8888/RGB888/RGB565/RGBA8888 (the formats
 *    with a defined pixel size; halo layer1 uses RGB888),
 *    CFB_ADDR is a bus-master address — the halo firmware programs the
 *    0x58xxxxxx global alias of the DTCM framebuffer (and the local
 *    0x20xxxxxx address on the swap path); both are mapped in the
 *    machine's system memory, so a plain address_space read resolves
 *    either — CFB_LENGTH holds the line pitch in [31:16], CFB_LINES
 *    the line count.
 *
 * The panel is portrait-mounted on the device, so the scanout is
 * presented rotated 90° counter-clockwise: framebuffer pixel (x, y)
 * lands at screen (y, panel_w - 1 - x).  The firmware renders for the
 * mounted orientation (glyphs are rotated in the framebuffer); without
 * this the window and screendumps show everything sideways.
 *
 * The frame tick is a 30 Hz virtual-clock timer, comfortably inside
 * the ticket's 30-60 Hz budget for the 8.76 MHz / 256x256 panel, and
 * independent of the UI backend so `-display none` still delivers the
 * LINE interrupts the driver's commit path needs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_HALO_CDC200 "halo-cdc200"
OBJECT_DECLARE_SIMPLE_TYPE(HaloCdcState, HALO_CDC200)

#define R_GLB_CTRL      0x018
#define R_SRCTRL        0x024
#define R_BP_CFG        0x00C
#define R_ACTW_CFG      0x010
#define R_IRQ_MASK0     0x034
#define R_IRQ_STATUS0   0x038
#define R_IRQ_CLEAR0    0x03C
#define R_POS_STAT      0x044

#define GLB_CTRL_CDC_EN 0x1
#define IRQ_LINE        0x1

/* Layer block: base + 0x100 * (1 + n) */
#define RL_CTRL         0x00C
#define RL_WIN_HPOS     0x010
#define RL_WIN_VPOS     0x014
#define RL_PIX_FORMAT   0x01C
#define RL_CFB_ADDR     0x034
#define RL_CFB_LENGTH   0x038
#define RL_CFB_LINES    0x03C

#define LN_CTRL_LAYER_EN 0x1

#define CDC_NUM_REGS    (0x1000 / 4)
#define CDC_NUM_LAYERS  2

#define CDC_FRAME_MS    33 /* ~30 Hz */

/* Panel dimensions sanity clamp (halo is 256x256) */
#define CDC_MAX_DIM     1024

struct HaloCdcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *console;
    QEMUTimer frame_timer;
    qemu_irq irq;

    uint32_t reg[CDC_NUM_REGS];
    uint32_t irq_status;
    uint32_t irq_mask;
};

static void halo_cdc_update_irq(HaloCdcState *s)
{
    qemu_set_irq(s->irq, !!(s->irq_status & s->irq_mask));
}

static bool halo_cdc_enabled(HaloCdcState *s)
{
    return s->reg[R_GLB_CTRL / 4] & GLB_CTRL_CDC_EN;
}

static void halo_cdc_panel_size(HaloCdcState *s, int *w, int *h)
{
    uint32_t bp = s->reg[R_BP_CFG / 4];
    uint32_t actw = s->reg[R_ACTW_CFG / 4];
    int pw = ((actw >> 16) & 0xffff) - ((bp >> 16) & 0xffff);
    int ph = (actw & 0xffff) - (bp & 0xffff);

    if (pw < 1 || pw > CDC_MAX_DIM || ph < 1 || ph > CDC_MAX_DIM) {
        pw = 256;
        ph = 256;
    }
    *w = pw;
    *h = ph;
}

/* Bytes per pixel for the PIX_FORMAT values with a defined size */
static int halo_cdc_bpp(uint32_t fmt)
{
    switch (fmt & 7) {
    case 0: /* ARGB8888 */
    case 3: /* RGBA8888 */
        return 4;
    case 1: /* RGB888 */
        return 3;
    case 2: /* RGB565 */
        return 2;
    default:
        return 0;
    }
}

static uint32_t halo_cdc_pixel(const uint8_t *p, uint32_t fmt)
{
    uint16_t v;

    switch (fmt & 7) {
    case 0: /* ARGB8888, LE: B G R A */
        return ldl_le_p(p) | 0xff000000u;
    case 3: /* RGBA8888, LE: A B G R */
        return (ldl_le_p(p) >> 8) | 0xff000000u;
    case 1: /* RGB888, LE: B G R */
        return 0xff000000u | (p[2] << 16) | (p[1] << 8) | p[0];
    case 2: /* RGB565 */
        v = lduw_le_p(p);
        return 0xff000000u |
               ((v >> 11) & 0x1f) << 19 |
               ((v >> 5) & 0x3f) << 10 |
               (v & 0x1f) << 3;
    default:
        return 0xff000000u;
    }
}

static void halo_cdc_draw_layer(HaloCdcState *s, DisplaySurface *surface,
                                int pw, int ph, int layer)
{
    uint32_t lbase = 0x100 * (1 + layer);
    uint32_t bp = s->reg[R_BP_CFG / 4];
    uint32_t fmt, addr, pitch, lines, hpos, vpos;
    int bpp, x0, y0, lw, lh;
    uint8_t linebuf[CDC_MAX_DIM * 4];

    if (!(s->reg[(lbase + RL_CTRL) / 4] & LN_CTRL_LAYER_EN)) {
        return;
    }
    fmt = s->reg[(lbase + RL_PIX_FORMAT) / 4];
    bpp = halo_cdc_bpp(fmt);
    if (!bpp) {
        return;
    }

    addr = s->reg[(lbase + RL_CFB_ADDR) / 4];
    pitch = (s->reg[(lbase + RL_CFB_LENGTH) / 4] >> 16) & 0xffff;
    lines = s->reg[(lbase + RL_CFB_LINES) / 4] & 0xffff;
    hpos = s->reg[(lbase + RL_WIN_HPOS) / 4];
    vpos = s->reg[(lbase + RL_WIN_VPOS) / 4];

    /* On-screen window: WIN start positions are accumulated timings */
    x0 = (hpos & 0xffff) - ((bp >> 16) & 0xffff) - 1;
    y0 = (vpos & 0xffff) - (bp & 0xffff) - 1;
    lw = ((hpos >> 16) & 0xffff) - (hpos & 0xffff) + 1;
    lh = ((vpos >> 16) & 0xffff) - (vpos & 0xffff) + 1;

    if (x0 < 0 || y0 < 0 || lw < 1 || x0 + lw > pw) {
        return;
    }
    lh = MIN(lh, (int)lines);
    lh = MIN(lh, ph - y0);

    /* Portrait mount: fb (x, y) -> screen (y, pw-1-x), so one fb line
     * lands in one screen column. */
    for (int y = 0; y < lh; y++) {
        if (address_space_read(&address_space_memory,
                               addr + (hwaddr)y * pitch,
                               MEMTXATTRS_UNSPECIFIED,
                               linebuf, (size_t)lw * bpp) != MEMTX_OK) {
            return;
        }
        for (int x = 0; x < lw; x++) {
            uint32_t *dst = (uint32_t *)(surface_data(surface) +
                                         (pw - 1 - (x0 + x)) *
                                         surface_stride(surface));

            dst[y0 + y] = halo_cdc_pixel(linebuf + (size_t)x * bpp, fmt);
        }
    }
}

static bool halo_cdc_gfx_update(void *opaque)
{
    HaloCdcState *s = opaque;
    DisplaySurface *surface;
    int pw, ph, sw, sh;

    halo_cdc_panel_size(s, &pw, &ph);
    /* Portrait mount: the screen is the framebuffer rotated 90° CCW */
    sw = ph;
    sh = pw;
    if (qemu_console_get_width(s->console, 0) != sw ||
        qemu_console_get_height(s->console, 0) != sh) {
        qemu_console_resize(s->console, sw, sh);
    }
    surface = qemu_console_surface(s->console);

    if (halo_cdc_enabled(s)) {
        uint32_t bg = 0xff000000u | (s->reg[0x2C / 4] & 0xffffff);

        for (int y = 0; y < sh; y++) {
            uint32_t *dst = (uint32_t *)(surface_data(surface) +
                                         y * surface_stride(surface));
            for (int x = 0; x < sw; x++) {
                dst[x] = bg;
            }
        }
        for (int layer = 0; layer < CDC_NUM_LAYERS; layer++) {
            halo_cdc_draw_layer(s, surface, pw, ph, layer);
        }
    } else {
        /* Disabled = blanked panel */
        for (int y = 0; y < sh; y++) {
            memset(surface_data(surface) + y * surface_stride(surface), 0,
                   (size_t)sw * 4);
        }
    }

    qemu_console_update(s->console, 0, 0, sw, sh);
    return true;
}

static void halo_cdc_invalidate(void *opaque)
{
    /* Everything redraws on the next gfx_update */
}

static const GraphicHwOps halo_cdc_gfx_ops = {
    .invalidate = halo_cdc_invalidate,
    .gfx_update = halo_cdc_gfx_update,
};

static void halo_cdc_frame_tick(void *opaque)
{
    HaloCdcState *s = opaque;

    if (!halo_cdc_enabled(s)) {
        return;
    }
    /* The scanline hits LINE_IRQ_POS once per frame */
    s->irq_status |= IRQ_LINE;
    halo_cdc_update_irq(s);
    timer_mod(&s->frame_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + CDC_FRAME_MS);
}

static uint64_t halo_cdc_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloCdcState *s = opaque;

    switch (offset) {
    case R_IRQ_STATUS0:
        return s->irq_status;
    case R_IRQ_MASK0:
        return s->irq_mask;
    case R_IRQ_CLEAR0:
    case R_SRCTRL: /* shadow reload completes instantly */
    case R_POS_STAT:
        return 0;
    default:
        return s->reg[offset / 4];
    }
}

static void halo_cdc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloCdcState *s = opaque;
    bool was_enabled = halo_cdc_enabled(s);

    switch (offset) {
    case R_IRQ_STATUS0:
        return;
    case R_IRQ_MASK0:
        s->irq_mask = value;
        halo_cdc_update_irq(s);
        return;
    case R_IRQ_CLEAR0:
        s->irq_status &= ~value;
        halo_cdc_update_irq(s);
        return;
    case R_SRCTRL:
        /* Registers apply immediately; nothing to commit */
        return;
    default:
        s->reg[offset / 4] = value;
        break;
    }

    if (offset == R_GLB_CTRL && halo_cdc_enabled(s) != was_enabled) {
        if (halo_cdc_enabled(s)) {
            timer_mod(&s->frame_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + CDC_FRAME_MS);
        } else {
            timer_del(&s->frame_timer);
        }
        qemu_console_hw_invalidate(s->console);
    }
}

static const MemoryRegionOps halo_cdc_ops = {
    .read = halo_cdc_read,
    .write = halo_cdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_cdc_reset(DeviceState *dev)
{
    HaloCdcState *s = HALO_CDC200(dev);

    timer_del(&s->frame_timer);
    memset(s->reg, 0, sizeof(s->reg));
    s->irq_status = 0;
    s->irq_mask = 0;
    halo_cdc_update_irq(s);
}

static void halo_cdc_init(Object *obj)
{
    HaloCdcState *s = HALO_CDC200(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_cdc_ops, s,
                          TYPE_HALO_CDC200, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void halo_cdc_realize(DeviceState *dev, Error **errp)
{
    HaloCdcState *s = HALO_CDC200(dev);

    timer_init_ms(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                  halo_cdc_frame_tick, s);
    s->console = qemu_graphic_console_create(dev, 0, &halo_cdc_gfx_ops, s);
    qemu_console_resize(s->console, 256, 256);
}

static void halo_cdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = halo_cdc_realize;
    dc->desc = "TES CDC200 display controller (halo)";
    device_class_set_legacy_reset(dc, halo_cdc_reset);
}

static const TypeInfo halo_cdc_types[] = {
    {
        .name = TYPE_HALO_CDC200,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloCdcState),
        .instance_init = halo_cdc_init,
        .class_init = halo_cdc_class_init,
    },
};

DEFINE_TYPES(halo_cdc_types)
