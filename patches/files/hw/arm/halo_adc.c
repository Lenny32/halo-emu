/*
 * Halo — Alif ADC12 (adc0 @ 0x49020000, alif,adc) feeding the VBAT
 * battery sensor.
 *
 * Contract from zephyr/drivers/adc/adc_alif.c + alif_vbat.c: a
 * single-shot conversion is started by writing ADC_CONTROL with bit 0
 * set while ADC_START_SRC bit 7 is up; completion must set the DONE1
 * interrupt (INTERRUPT bit 1, W1C, IRQ 154), latch the converted
 * channel number into the read-only ADC_SEL, and place the averaged
 * 12-bit value in ADC_SAMPLE_REG[channel] (0x50 + 4*ch).  The repeat
 * path re-arms from inside the ISR with a read-modify-OR that never
 * clears bit 0, so any bit-0 write triggers — no edge detection.
 * Everything else (sequencer, thresholds, ADC_REG1, the comparator and
 * AON windows) is plain register storage; the driver has no readback
 * checks or polling loops.
 *
 * The sample value is a qdev property ("battery-raw", 0..4095).  The
 * firmware computes mV = raw * 1800 / 4095 scaled by the 2.4k/1k
 * divider, i.e. Vbat_mV = raw * 4320 / 4095; the default 3698 reads as
 * a healthy 3.9 V battery (66%).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_ADC "halo-adc"
OBJECT_DECLARE_SIMPLE_TYPE(HaloAdcState, HALO_ADC)

#define R_ADC_START_SRC      0x00
#define R_ADC_COMP_THRESH_A  0x04
#define R_ADC_COMP_THRESH_B  0x08
#define R_ADC_CLK_DIVISOR    0x0C
#define R_ADC_INTERRUPT      0x10 /* W1C status: b0 DONE0 b1 DONE1 b2/3 CMP */
#define R_ADC_INTERRUPT_MASK 0x14 /* 1 = masked */
#define R_ADC_SAMPLE_WIDTH   0x18
#define R_ADC_AVG_NUM        0x20
#define R_ADC_SHIFT_CONTROL  0x24
#define R_ADC_CONTROL        0x30 /* bit0 = single-shot start */
#define R_ADC_SEQUENCER_CTRL 0x34 /* [15:12] init channel, [8:0] mask */
#define R_ADC_REG1           0x38
#define R_ADC_SEL            0x3C /* RO: channel just converted */
#define R_ADC_SAMPLE_BASE    0x50 /* + 4*channel, 9 channels */

#define ADC_START_ENABLE     (1 << 7)
#define ADC_SINGLE_SHOT      (1 << 0)
#define ADC_INTR_DONE1       (1 << 1)
#define ADC_NUM_CHANNELS     9

struct HaloAdcState {
    SysBusDevice parent_obj;

    MemoryRegion adc_iomem;  /* mmio[0] @ 0x49020000, 0x1000 */
    MemoryRegion comp_iomem; /* mmio[1] @ 0x49023000, 0x100 */
    MemoryRegion aon_iomem;  /* mmio[2] @ 0x1A604000, 0x100 */
    qemu_irq done1_irq;      /* IRQ 154 (single_shot_intr) */

    uint32_t start_src;
    uint32_t intr_status;
    uint32_t intr_mask;
    uint32_t control;
    uint32_t sequencer_ctrl;
    uint32_t sel;
    uint32_t sample[ADC_NUM_CHANNELS];
    uint32_t cfg[0x40 / 4];    /* thresholds, divisor, width, ... */
    uint32_t comp_regs[0x100 / 4];
    uint32_t aon_regs[0x100 / 4];

    uint32_t battery_raw;
};

static void halo_adc_update_irq(HaloAdcState *s)
{
    qemu_set_irq(s->done1_irq,
                 (s->intr_status & ADC_INTR_DONE1) &&
                 !(s->intr_mask & ADC_INTR_DONE1));
}

static void halo_adc_convert(HaloAdcState *s)
{
    unsigned ch = (s->sequencer_ctrl >> 12) & 0xF;

    if (ch >= ADC_NUM_CHANNELS) {
        return;
    }
    s->sample[ch] = s->battery_raw & 0xFFF;
    s->sel = ch;
    s->intr_status |= ADC_INTR_DONE1;
    halo_adc_update_irq(s);
}

static uint64_t halo_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloAdcState *s = opaque;

    switch (offset) {
    case R_ADC_START_SRC:
        return s->start_src;
    case R_ADC_INTERRUPT:
        return s->intr_status;
    case R_ADC_INTERRUPT_MASK:
        return s->intr_mask;
    case R_ADC_CONTROL:
        return s->control;
    case R_ADC_SEQUENCER_CTRL:
        return s->sequencer_ctrl;
    case R_ADC_SEL:
        return s->sel;
    case R_ADC_SAMPLE_BASE ...
         (R_ADC_SAMPLE_BASE + 4 * (ADC_NUM_CHANNELS - 1)):
        return s->sample[(offset - R_ADC_SAMPLE_BASE) / 4];
    default:
        if (offset < 0x40) {
            return s->cfg[offset / 4];
        }
        return 0;
    }
}

static void halo_adc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloAdcState *s = opaque;

    switch (offset) {
    case R_ADC_START_SRC:
        s->start_src = value;
        break;
    case R_ADC_INTERRUPT: /* W1C */
        s->intr_status &= ~value;
        halo_adc_update_irq(s);
        break;
    case R_ADC_INTERRUPT_MASK:
        s->intr_mask = value;
        halo_adc_update_irq(s);
        break;
    case R_ADC_CONTROL:
        s->control = value;
        if ((value & ADC_SINGLE_SHOT) && (s->start_src & ADC_START_ENABLE)) {
            halo_adc_convert(s);
        }
        break;
    case R_ADC_SEQUENCER_CTRL:
        s->sequencer_ctrl = value;
        break;
    case R_ADC_SEL:
        break; /* read-only */
    default:
        if (offset < 0x40) {
            s->cfg[offset / 4] = value;
        }
        break;
    }
}

static uint64_t halo_adc_comp_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloAdcState *s = opaque;

    return s->comp_regs[offset / 4];
}

static void halo_adc_comp_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    HaloAdcState *s = opaque;

    s->comp_regs[offset / 4] = value;
}

static uint64_t halo_adc_aon_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloAdcState *s = opaque;

    return s->aon_regs[offset / 4];
}

static void halo_adc_aon_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    HaloAdcState *s = opaque;

    s->aon_regs[offset / 4] = value;
}

static const MemoryRegionOps halo_adc_ops = {
    .read = halo_adc_read,
    .write = halo_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps halo_adc_comp_ops = {
    .read = halo_adc_comp_read,
    .write = halo_adc_comp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps halo_adc_aon_ops = {
    .read = halo_adc_aon_read,
    .write = halo_adc_aon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_adc_reset(DeviceState *dev)
{
    HaloAdcState *s = HALO_ADC(dev);

    s->start_src = 0;
    s->intr_status = 0;
    s->intr_mask = 0xF;
    s->control = 0;
    s->sequencer_ctrl = 0;
    s->sel = 0;
    memset(s->sample, 0, sizeof(s->sample));
    memset(s->cfg, 0, sizeof(s->cfg));
    memset(s->comp_regs, 0, sizeof(s->comp_regs));
    memset(s->aon_regs, 0, sizeof(s->aon_regs));
    halo_adc_update_irq(s);
}

static void halo_adc_init(Object *obj)
{
    HaloAdcState *s = HALO_ADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->adc_iomem, obj, &halo_adc_ops, s,
                          "halo-adc", 0x1000);
    sysbus_init_mmio(sbd, &s->adc_iomem);
    memory_region_init_io(&s->comp_iomem, obj, &halo_adc_comp_ops, s,
                          "halo-adc.comp", 0x100);
    sysbus_init_mmio(sbd, &s->comp_iomem);
    memory_region_init_io(&s->aon_iomem, obj, &halo_adc_aon_ops, s,
                          "halo-adc.aon", 0x100);
    sysbus_init_mmio(sbd, &s->aon_iomem);
    sysbus_init_irq(sbd, &s->done1_irq);
}

/* raw = Vbat_mV * 4095 / 4320; 3698 == 3.9 V through the 2.4k/1k divider */
static const Property halo_adc_properties[] = {
    DEFINE_PROP_UINT32("battery-raw", HaloAdcState, battery_raw, 3698),
};

static void halo_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_adc_reset);
    device_class_set_props(dc, halo_adc_properties);
}

static const TypeInfo halo_adc_types[] = {
    {
        .name = TYPE_HALO_ADC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloAdcState),
        .instance_init = halo_adc_init,
        .class_init = halo_adc_class_init,
    },
};

DEFINE_TYPES(halo_adc_types)
