/*
 * Halo — DesignWare APB GPIO bank (snps,designware-gpio), port A only.
 *
 * Eleven instances on the Balletto B1: gpio0..9 @ 0x49000000+n*0x1000
 * and LPGPIO @ 0x42002000.  The Alif integration gives every pin its
 * own NVIC line, all bound to the same ISR, which resolves the pin
 * purely from INTSTATUS and acks with PORTA_EOI — so this model raises
 * one output IRQ per pin from `intstatus = raw & inten & ~intmask`.
 * Contract from zephyr/drivers/gpio/gpio_dw.c:
 *  - every config register is read-modify-write and must read back
 *    (DR, DDR, INTEN, INTMASK, INTTYPE_LEVEL, INT_POLARITY,
 *    INT_BOTHEDGE, DEBOUNCE, LS_SYNC);
 *  - EXT_PORTA is read even for outputs: (DR & DDR) | (input & ~DDR);
 *  - PORTA_EOI (W1C) clears latched edge interrupts.
 * External input values arrive on qdev GPIO-in lines (named "in", one
 * per pin) — the inputs-and-controls ticket drives them.  The reset
 * level of each input comes from the "in-default" bitmask property:
 * active-low lines with an external pull-up (the button on LPGPIO pin
 * 1) must idle high or the firmware sees them held active and a
 * level-type interrupt storms.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_DWGPIO "halo-dwgpio"
OBJECT_DECLARE_SIMPLE_TYPE(HaloDwGpioState, HALO_DWGPIO)

#define DWGPIO_MAX_PINS 8

#define R_SWPORTA_DR      0x00
#define R_SWPORTA_DDR     0x04
#define R_INTEN           0x30
#define R_INTMASK         0x34
#define R_INTTYPE_LEVEL   0x38 /* 1 = edge, 0 = level */
#define R_INT_POLARITY    0x3C /* 1 = high / rising */
#define R_INTSTATUS       0x40
#define R_RAW_INTSTATUS   0x44
#define R_PORTA_DEBOUNCE  0x48
#define R_PORTA_EOI       0x4C /* W1C */
#define R_EXT_PORTA       0x50
#define R_LS_SYNC         0x60
#define R_INT_BOTHEDGE    0x68
#define R_VER_ID_CODE     0x6C

struct HaloDwGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[DWGPIO_MAX_PINS];

    uint32_t dr;
    uint32_t ddr;
    uint32_t inten;
    uint32_t intmask;
    uint32_t inttype_level;
    uint32_t int_polarity;
    uint32_t int_bothedge;
    uint32_t debounce;
    uint32_t ls_sync;
    uint32_t ext_in;       /* external input levels */
    uint32_t raw_int;      /* latched edges + live levels */

    uint32_t ngpios;
    uint32_t in_default;   /* reset level of the external inputs */
};

static uint32_t halo_dwgpio_ext(HaloDwGpioState *s)
{
    return (s->dr & s->ddr) | (s->ext_in & ~s->ddr);
}

/* A pin is edge-sensitive when INTTYPE_LEVEL is set — or when
 * INT_BOTHEDGE is: for GPIO_INT_EDGE_BOTH the Zephyr driver sets only
 * the latter and leaves INTTYPE_LEVEL at 0, and the DW IP gives
 * INT_BOTHEDGE precedence. */
static uint32_t halo_dwgpio_edge_mode(HaloDwGpioState *s)
{
    return s->inttype_level | s->int_bothedge;
}

/* level-type pins track the (polarity-adjusted) input; edges stay latched */
static void halo_dwgpio_update(HaloDwGpioState *s)
{
    uint32_t levels = halo_dwgpio_ext(s);
    uint32_t edge_mode = halo_dwgpio_edge_mode(s);
    uint32_t level_match = (s->int_polarity & levels) |
                           (~s->int_polarity & ~levels);
    s->raw_int = (s->raw_int & edge_mode & s->inten) |
                 (~edge_mode & s->inten & level_match);

    uint32_t status = s->raw_int & s->inten & ~s->intmask;

    for (unsigned i = 0; i < s->ngpios; i++) {
        qemu_set_irq(s->irq[i], (status >> i) & 1);
    }
}

static void halo_dwgpio_input_set(void *opaque, int pin, int level)
{
    HaloDwGpioState *s = opaque;
    uint32_t old = halo_dwgpio_ext(s);
    uint32_t bit = 1u << pin;

    s->ext_in = (s->ext_in & ~bit) | (level ? bit : 0);

    /* edge detection on the effective pin level */
    uint32_t new = halo_dwgpio_ext(s);
    uint32_t rising = new & ~old;
    uint32_t falling = old & ~new;
    uint32_t edge = (s->int_bothedge & (rising | falling)) |
                    (~s->int_bothedge &
                     ((s->int_polarity & rising) |
                      (~s->int_polarity & falling)));

    s->raw_int |= edge & halo_dwgpio_edge_mode(s) & s->inten & bit;
    halo_dwgpio_update(s);
}

static uint64_t halo_dwgpio_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloDwGpioState *s = opaque;

    switch (offset) {
    case R_SWPORTA_DR:
        return s->dr;
    case R_SWPORTA_DDR:
        return s->ddr;
    case R_INTEN:
        return s->inten;
    case R_INTMASK:
        return s->intmask;
    case R_INTTYPE_LEVEL:
        return s->inttype_level;
    case R_INT_POLARITY:
        return s->int_polarity;
    case R_INTSTATUS:
        return s->raw_int & s->inten & ~s->intmask;
    case R_RAW_INTSTATUS:
        return s->raw_int;
    case R_PORTA_DEBOUNCE:
        return s->debounce;
    case R_EXT_PORTA:
        return halo_dwgpio_ext(s);
    case R_LS_SYNC:
        return s->ls_sync;
    case R_INT_BOTHEDGE:
        return s->int_bothedge;
    case R_VER_ID_CODE:
        return 0x3231342A; /* '*412' */
    default:
        return 0;
    }
}

static void halo_dwgpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    HaloDwGpioState *s = opaque;

    switch (offset) {
    case R_SWPORTA_DR:
        s->dr = value;
        break;
    case R_SWPORTA_DDR:
        s->ddr = value;
        break;
    case R_INTEN:
        s->inten = value;
        break;
    case R_INTMASK:
        s->intmask = value;
        break;
    case R_INTTYPE_LEVEL:
        s->inttype_level = value;
        break;
    case R_INT_POLARITY:
        s->int_polarity = value;
        break;
    case R_PORTA_DEBOUNCE:
        s->debounce = value;
        break;
    case R_PORTA_EOI:
        s->raw_int &= ~value;
        break;
    case R_LS_SYNC:
        s->ls_sync = value;
        break;
    case R_INT_BOTHEDGE:
        s->int_bothedge = value;
        break;
    default:
        return;
    }
    halo_dwgpio_update(s);
}

static const MemoryRegionOps halo_dwgpio_ops = {
    .read = halo_dwgpio_read,
    .write = halo_dwgpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_dwgpio_reset(DeviceState *dev)
{
    HaloDwGpioState *s = HALO_DWGPIO(dev);

    s->dr = 0;
    s->ddr = 0;
    s->inten = 0;
    s->intmask = 0;
    s->inttype_level = 0;
    s->int_polarity = 0;
    s->int_bothedge = 0;
    s->debounce = 0;
    s->ls_sync = 0;
    s->ext_in = s->in_default;
    s->raw_int = 0;
    halo_dwgpio_update(s);
}

static void halo_dwgpio_init(Object *obj)
{
    HaloDwGpioState *s = HALO_DWGPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_dwgpio_ops, s,
                          "halo-dwgpio", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < DWGPIO_MAX_PINS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), halo_dwgpio_input_set, "in",
                            DWGPIO_MAX_PINS);
}

static const Property halo_dwgpio_properties[] = {
    DEFINE_PROP_UINT32("ngpios", HaloDwGpioState, ngpios, DWGPIO_MAX_PINS),
    DEFINE_PROP_UINT32("in-default", HaloDwGpioState, in_default, 0),
};

static void halo_dwgpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_dwgpio_reset);
    device_class_set_props(dc, halo_dwgpio_properties);
}

static const TypeInfo halo_dwgpio_types[] = {
    {
        .name = TYPE_HALO_DWGPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloDwGpioState),
        .instance_init = halo_dwgpio_init,
        .class_init = halo_dwgpio_class_init,
    },
};

DEFINE_TYPES(halo_dwgpio_types)
