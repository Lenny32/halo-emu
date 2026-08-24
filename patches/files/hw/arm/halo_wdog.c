/*
 * Halo — CMSDK APB watchdog stub @ 0x40100000 (arm,cmsdk-watchdog).
 *
 * The firmware arms it (LOCK unlock 0x1ACCE551, LOAD, CTRL INTEN|RESEN)
 * and feeds it every second (INTCLR + LOAD rewrite) from a coop thread.
 * On hardware an expiry raises the NMI (CONFIG_RUNTIME_NMI); here the
 * counter never ticks so it can never fire — deliberate: emulation
 * stalls (host scheduling, gdb) must not reboot the guest.  Registers
 * are plain state so a later ticket can surface them; the status
 * registers read 0, which is what wdog_cmsdk_apb_has_fired() checks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_WDOG "halo-wdog"
OBJECT_DECLARE_SIMPLE_TYPE(HaloWdogState, HALO_WDOG)

#define R_WDOG_LOAD        0x000
#define R_WDOG_VALUE       0x004
#define R_WDOG_CTRL        0x008
#define R_WDOG_INTCLR      0x00C
#define R_WDOG_RAWINTSTAT  0x010
#define R_WDOG_MASKINTSTAT 0x014
#define R_WDOG_LOCK        0xC00

#define WDOG_UNLOCK_VALUE  0x1ACCE551

struct HaloWdogState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t load;
    uint32_t ctrl;
    uint32_t locked;
};

static uint64_t halo_wdog_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloWdogState *s = opaque;

    switch (offset) {
    case R_WDOG_LOAD:
        return s->load;
    case R_WDOG_VALUE:
        return s->load; /* the counter never decrements */
    case R_WDOG_CTRL:
        return s->ctrl;
    case R_WDOG_RAWINTSTAT:
    case R_WDOG_MASKINTSTAT:
        return 0; /* never fired */
    case R_WDOG_LOCK:
        return s->locked;
    default:
        return 0;
    }
}

static void halo_wdog_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    HaloWdogState *s = opaque;

    if (s->locked && offset != R_WDOG_LOCK) {
        return;
    }

    switch (offset) {
    case R_WDOG_LOAD:
        s->load = value;
        break;
    case R_WDOG_CTRL:
        s->ctrl = value & 3;
        break;
    case R_WDOG_INTCLR:
        break;
    case R_WDOG_LOCK:
        s->locked = (value != WDOG_UNLOCK_VALUE);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps halo_wdog_ops = {
    .read = halo_wdog_read,
    .write = halo_wdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_wdog_reset(DeviceState *dev)
{
    HaloWdogState *s = HALO_WDOG(dev);

    s->load = 0xFFFFFFFF;
    s->ctrl = 0;
    s->locked = 1;
}

static void halo_wdog_init(Object *obj)
{
    HaloWdogState *s = HALO_WDOG(obj);

    memory_region_init_io(&s->iomem, obj, &halo_wdog_ops, s,
                          "halo-wdog", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void halo_wdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_wdog_reset);
}

static const TypeInfo halo_wdog_types[] = {
    {
        .name = TYPE_HALO_WDOG,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloWdogState),
        .instance_init = halo_wdog_init,
        .class_init = halo_wdog_class_init,
    },
};

DEFINE_TYPES(halo_wdog_types)
