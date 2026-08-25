/*
 * Halo — Alif UTIMER PWM sink (utimer3 @ 0x48004000 + global block
 * @ 0x48000000, alif,utimer / alif,pwm).
 *
 * The LED lives on utimer3 channel 0 (compare A): Zephyr's
 * pwm_alif_utimer.c programs the period into CNTR_PTR (0xA4) and the
 * duty into COMPARE_A (0xD0), gates the output with COMPARE_CTRL_A
 * (0x8C) bit 8 (DRIVER_EN, bit 9 = idle-high when disabled) and the
 * global active-low GLB_DRIVER_OEN.  The driver never reads a hardware
 * status except GLB_CNTR_RUNNING, and never polls — so the timer block
 * is plain register storage (every RMW readback is satisfied), and the
 * global block derives RUNNING from the START/STOP write-1 registers.
 *
 * LED readout (ticket 0031): read-only QOM properties "led-duty"
 * (COMPARE_A), "led-period" (CNTR_PTR) and "led-on" (COMPARE_CTRL_A
 * bit 8, DRIVER_EN) — the machine forwards them for the control
 * socket's `led?` verb.  Brightness = duty/period while led-on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_UTIMER "halo-utimer"
OBJECT_DECLARE_SIMPLE_TYPE(HaloUtimerState, HALO_UTIMER)

#define UTIMER_BLOCK_SIZE  0x1000
#define UTIMER_NUM_REGS    (UTIMER_BLOCK_SIZE / 4)

/* Global block (0x24 bytes, shared by timers 0..3 on hardware) */
#define R_GLB_CNTR_START   0x00 /* write-1-to-start */
#define R_GLB_CNTR_STOP    0x04 /* write-1-to-stop */
#define R_GLB_CNTR_CLEAR   0x08
#define R_GLB_CNTR_RUNNING 0x0C /* read-only status */
#define R_GLB_DRIVER_OEN   0x10 /* active-low output enable, plain R/W */
#define R_GLB_CLOCK_ENABLE 0x20 /* plain R/W */
#define GLB_BLOCK_SIZE     0x24

/* Timer-block registers the LED PWM readout uses */
#define R_CNTR_PTR         0xA4 /* PWM period */
#define R_COMPARE_CTRL_A   0x8C /* bit 8 = DRIVER_EN, bit 9 = idle-high */
#define R_COMPARE_A        0xD0 /* PWM duty */
#define COMPARE_CTRL_DRIVER_EN (1 << 8)

struct HaloUtimerState {
    SysBusDevice parent_obj;

    MemoryRegion timer_iomem; /* mmio[0]: timer block */
    MemoryRegion glb_iomem;   /* mmio[1]: global block */

    uint32_t regs[UTIMER_NUM_REGS];
    uint32_t glb_running;
    uint32_t glb_driver_oen;
    uint32_t glb_clock_enable;
};

static uint64_t halo_utimer_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloUtimerState *s = opaque;

    return s->regs[offset / 4];
}

static void halo_utimer_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    HaloUtimerState *s = opaque;

    s->regs[offset / 4] = value;
}

static uint64_t halo_utimer_glb_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    HaloUtimerState *s = opaque;

    switch (offset) {
    case R_GLB_CNTR_RUNNING:
        return s->glb_running;
    case R_GLB_DRIVER_OEN:
        return s->glb_driver_oen;
    case R_GLB_CLOCK_ENABLE:
        return s->glb_clock_enable;
    default:
        return 0;
    }
}

static void halo_utimer_glb_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    HaloUtimerState *s = opaque;

    switch (offset) {
    case R_GLB_CNTR_START:
        s->glb_running |= value;
        break;
    case R_GLB_CNTR_STOP:
        s->glb_running &= ~value;
        break;
    case R_GLB_DRIVER_OEN:
        s->glb_driver_oen = value;
        break;
    case R_GLB_CLOCK_ENABLE:
        s->glb_clock_enable = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps halo_utimer_ops = {
    .read = halo_utimer_read,
    .write = halo_utimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps halo_utimer_glb_ops = {
    .read = halo_utimer_glb_read,
    .write = halo_utimer_glb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_utimer_reset(DeviceState *dev)
{
    HaloUtimerState *s = HALO_UTIMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->glb_running = 0;
    s->glb_driver_oen = 0xFFFFFFFF; /* active-low: all outputs disabled */
    s->glb_clock_enable = 0;
}

static bool halo_utimer_get_led_on(Object *obj, Error **errp)
{
    HaloUtimerState *s = HALO_UTIMER(obj);

    return (s->regs[R_COMPARE_CTRL_A / 4] & COMPARE_CTRL_DRIVER_EN) != 0;
}

static void halo_utimer_init(Object *obj)
{
    HaloUtimerState *s = HALO_UTIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->timer_iomem, obj, &halo_utimer_ops, s,
                          "halo-utimer", UTIMER_BLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->timer_iomem);
    memory_region_init_io(&s->glb_iomem, obj, &halo_utimer_glb_ops, s,
                          "halo-utimer.global", GLB_BLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->glb_iomem);

    object_property_add_uint32_ptr(obj, "led-duty",
                                   &s->regs[R_COMPARE_A / 4],
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "led-period",
                                   &s->regs[R_CNTR_PTR / 4],
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "led-on", halo_utimer_get_led_on, NULL);
}

static void halo_utimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_utimer_reset);
}

static const TypeInfo halo_utimer_types[] = {
    {
        .name = TYPE_HALO_UTIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloUtimerState),
        .instance_init = halo_utimer_init,
        .class_init = halo_utimer_class_init,
    },
};

DEFINE_TYPES(halo_utimer_types)
