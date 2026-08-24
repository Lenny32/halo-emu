/*
 * Halo — DesignWare APB RTC @ 0x42000000 (snps,dw-apb-rtc, 32768 Hz,
 * IRQ 58): free-running counter + match interrupt.
 *
 * This is the firmware's `zephyr,cortex-m-idle-timer`: on tickless idle
 * the systick driver programs CMR = CCVR + delta with CCR.IEN set and
 * CCR.MASK clear, WFIs, and the match interrupt (or any other wakeup)
 * ends the nap; the ISR reads EOI to clear.  Contract quirks from
 * counter_dw_rtc.c:
 *  - CCVR must be strictly monotonic and must count regardless of
 *    CCR.EN: counter_start() is never called on the idle path and the
 *    driver's CCR shadow starts as noinit garbage.  A CCVR that ever
 *    appears to decrease makes cortex_m_systick.c call the driver's
 *    missing get_top_value (NULL deref).
 *  - The prescaler is 0 in the devicetree, so CPSR is never written;
 *    the counter ticks at the ref clock directly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_RTC "halo-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(HaloRtcState, HALO_RTC)

#define RTC_FREQ_HZ  32768

#define R_RTC_CCVR   0x00 /* current counter value (RO) */
#define R_RTC_CMR    0x04 /* counter match */
#define R_RTC_CLR    0x08 /* counter load */
#define R_RTC_CCR    0x0C /* control */
#define R_RTC_STAT   0x10 /* masked interrupt status */
#define R_RTC_RSTAT  0x14 /* raw interrupt status */
#define R_RTC_EOI    0x18 /* read to clear */
#define R_RTC_VER    0x1C
#define R_RTC_CPSR   0x20 /* prescaler (unused: DT prescaler = 0) */

#define CCR_IEN      (1 << 0)
#define CCR_MASK     (1 << 1)
#define CCR_EN       (1 << 2)

struct HaloRtcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer timer;

    /* counter = base_count + ticks elapsed since base_ns */
    uint32_t base_count;
    int64_t base_ns;

    uint32_t cmr;
    uint32_t ccr;
    uint32_t raw_stat;
};

static uint32_t halo_rtc_count(HaloRtcState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return s->base_count +
           (uint32_t)muldiv64(now - s->base_ns, RTC_FREQ_HZ,
                              NANOSECONDS_PER_SECOND);
}

static bool halo_rtc_int_armed(HaloRtcState *s)
{
    return (s->ccr & CCR_IEN) && !(s->ccr & CCR_MASK);
}

static void halo_rtc_update_irq(HaloRtcState *s)
{
    qemu_set_irq(s->irq, s->raw_stat && halo_rtc_int_armed(s));
}

static void halo_rtc_rearm(HaloRtcState *s)
{
    if (!halo_rtc_int_armed(s) || s->raw_stat) {
        timer_del(&s->timer);
        return;
    }

    /* fire when the counter reaches CMR (32-bit wrap-around distance) */
    uint32_t delta = s->cmr - halo_rtc_count(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_mod(&s->timer, now + muldiv64(delta ? delta : 1,
                                        NANOSECONDS_PER_SECOND, RTC_FREQ_HZ));
}

static void halo_rtc_match(void *opaque)
{
    HaloRtcState *s = opaque;

    s->raw_stat = 1;
    halo_rtc_update_irq(s);
}

static uint64_t halo_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloRtcState *s = opaque;

    switch (offset) {
    case R_RTC_CCVR:
        return halo_rtc_count(s);
    case R_RTC_CMR:
        return s->cmr;
    case R_RTC_CLR:
        return s->base_count;
    case R_RTC_CCR:
        return s->ccr;
    case R_RTC_STAT:
        return (s->raw_stat && halo_rtc_int_armed(s)) ? 1 : 0;
    case R_RTC_RSTAT:
        return s->raw_stat;
    case R_RTC_EOI:
        s->raw_stat = 0;
        halo_rtc_update_irq(s);
        halo_rtc_rearm(s);
        return 0;
    case R_RTC_VER:
        return 0x3230312A; /* '*102' */
    default:
        return 0;
    }
}

static void halo_rtc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloRtcState *s = opaque;

    switch (offset) {
    case R_RTC_CMR:
        s->cmr = value;
        halo_rtc_rearm(s);
        break;
    case R_RTC_CLR:
        /*
         * Reload the counter.  The idle path never does this; if the
         * pm path does (counter_start writes 0), keep counting from
         * the new value.
         */
        s->base_count = value;
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        halo_rtc_rearm(s);
        break;
    case R_RTC_CCR:
        s->ccr = value;
        halo_rtc_update_irq(s);
        halo_rtc_rearm(s);
        break;
    case R_RTC_CPSR:
        break; /* prescaler unused (DT prescaler = 0) */
    default:
        break;
    }
}

static const MemoryRegionOps halo_rtc_ops = {
    .read = halo_rtc_read,
    .write = halo_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_rtc_reset(DeviceState *dev)
{
    HaloRtcState *s = HALO_RTC(dev);

    timer_del(&s->timer);
    s->base_count = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->cmr = 0;
    s->ccr = 0;
    s->raw_stat = 0;
    halo_rtc_update_irq(s);
}

static void halo_rtc_init(Object *obj)
{
    HaloRtcState *s = HALO_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_rtc_ops, s,
                          "halo-rtc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, halo_rtc_match, s);
}

static void halo_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_rtc_reset);
}

static const TypeInfo halo_rtc_types[] = {
    {
        .name = TYPE_HALO_RTC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloRtcState),
        .instance_init = halo_rtc_init,
        .class_init = halo_rtc_class_init,
    },
};

DEFINE_TYPES(halo_rtc_types)
