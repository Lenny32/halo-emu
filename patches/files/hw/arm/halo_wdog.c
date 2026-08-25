/*
 * Halo — CMSDK APB watchdog stub @ 0x40100000 (arm,cmsdk-watchdog).
 *
 * The firmware arms it (LOCK unlock 0x1ACCE551, LOAD, CTRL INTEN|RESEN)
 * and feeds it every second (INTCLR + LOAD rewrite) from a coop thread.
 * The counter never ticks on its own — deliberate: emulation stalls
 * (host scheduling, gdb) must not reboot the guest.
 *
 * Expiry is injectable instead (ticket 0031): the QOM property "fire"
 * (qom-set <dev> fire true) models one full CMSDK timeout sequence —
 *  1. RAWINTSTAT is latched and, with CTRL.INTEN set, the NMI line is
 *     raised.  Zephyr's wdt_cmsdk_apb ISR runs off the NMI, reads
 *     MASKINTSTAT (nonzero = "has fired") and calls the app callback,
 *     which stores the firmware's watchdog-fired __noinit magic.
 *  2. 200 ms (virtual) later the model resets the machine — hardware's
 *     RESEN second-timeout SoC reset.  The firmware's feed thread would
 *     avert that on hardware by clearing INTCLR in time; the injected
 *     expiry resets unconditionally so the test path is deterministic.
 *     The reset preserves the TCMs (halo_sram_preserve_next_reset) —
 *     a watchdog reset is warm, unlike the SE's TCM power-cycle — so
 *     halo_watchdog_has_fired() sees the magic on the next boot.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "qom/object.h"
#include "halo.h"

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
#define WDOG_CTRL_INTEN    (1 << 0)
#define WDOG_CTRL_RESEN    (1 << 1)

/* virtual-time gap between the NMI and the second-timeout reset: long
 * enough for the guest's NMI handler to store its magic, short enough
 * that the 1 Hz feed thread cannot legitimately intervene first */
#define WDOG_RESET_DELAY_MS 200

struct HaloWdogState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq nmi;
    QEMUTimer *reset_timer;

    uint32_t load;
    uint32_t ctrl;
    uint32_t locked;
    uint32_t rawintstat;
};

static void halo_wdog_update_nmi(HaloWdogState *s)
{
    qemu_set_irq(s->nmi, s->rawintstat && (s->ctrl & WDOG_CTRL_INTEN));
}

static void halo_wdog_reset_expired(void *opaque)
{
    halo_sram_preserve_next_reset();
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void halo_wdog_fire(HaloWdogState *s)
{
    s->rawintstat = 1;
    halo_wdog_update_nmi(s);
    timer_mod(s->reset_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + WDOG_RESET_DELAY_MS);
}

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
        return s->rawintstat;
    case R_WDOG_MASKINTSTAT:
        return s->rawintstat && (s->ctrl & WDOG_CTRL_INTEN);
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
        halo_wdog_update_nmi(s);
        break;
    case R_WDOG_INTCLR: /* feed: interrupt acknowledged */
        s->rawintstat = 0;
        halo_wdog_update_nmi(s);
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

static bool halo_wdog_get_fire(Object *obj, Error **errp)
{
    HaloWdogState *s = HALO_WDOG(obj);

    return s->rawintstat != 0;
}

static void halo_wdog_set_fire(Object *obj, bool value, Error **errp)
{
    HaloWdogState *s = HALO_WDOG(obj);

    if (!value) {
        error_setg(errp, "halo-wdog: fire only accepts true");
        return;
    }
    halo_wdog_fire(s);
}

static void halo_wdog_reset(DeviceState *dev)
{
    HaloWdogState *s = HALO_WDOG(dev);

    s->load = 0xFFFFFFFF;
    s->ctrl = 0;
    s->locked = 1;
    s->rawintstat = 0;
    timer_del(s->reset_timer);
    halo_wdog_update_nmi(s);
}

static void halo_wdog_init(Object *obj)
{
    HaloWdogState *s = HALO_WDOG(obj);

    memory_region_init_io(&s->iomem, obj, &halo_wdog_ops, s,
                          "halo-wdog", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->nmi);
    s->reset_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                  halo_wdog_reset_expired, s);
    object_property_add_bool(obj, "fire",
                             halo_wdog_get_fire, halo_wdog_set_fire);
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
