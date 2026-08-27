/*
 * Halo — BMA580 accelerometer model (I2C0 @ 0x18, INT1 on gpio3.2).
 *
 * A register file plus the handful of behaviours the Bosch driver
 * demands before it will probe (halo_i2c_regfile.c is not enough):
 *
 *  - CHIP_ID (0x00) reads 0xC4, or bma580_init() returns
 *    BMA5_E_DEV_NOT_FOUND (bma580_features.c:108).
 *  - HEALTH_STATUS (0x02) low nibble reads 0x0F. bma580_init() polls it
 *    in an *unbounded* for(;;) loop after leaving suspend
 *    (bma580_features.c:122-137), so any other value hangs the guest
 *    rather than failing it.
 *  - ACC_DATA_0..5 (0x18-0x1D) serve the injected sample as
 *    little-endian int16 per axis, the layout reassembled at
 *    bma580_driver.c:239-241; TEMP_DATA (0x1E) follows it.
 *  - INT_STATUS_INT1_0/1 (0x12/0x13) report injected tap and
 *    data-ready events and are write-1-to-clear, matching the ISR's
 *    read-then-write-back at bma580_driver.c:146-196. INT1 is asserted
 *    while any status bit is pending.
 *
 * Everything else is plain storage, which covers the driver's
 * read-modify-write of ACC_CONF/INT_MAP and its feature-engine DMA
 * (FEATURE_DATA_ADDR/TX): reading back zeros there is harmless because
 * taps are injected directly rather than computed by a feature engine.
 *
 * Injection: "accel-x/y/z" (raw LSB counts) and "tap" (1 = single,
 * 2 = double, 3 = triple), driven from the machine (ticket 0037).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"

#define TYPE_HALO_BMA580 "halo-bma580"
OBJECT_DECLARE_SIMPLE_TYPE(HaloBma580State, HALO_BMA580)

#define BMA_REG_CHIP_ID        0x00
#define BMA_REG_HEALTH_STATUS  0x02
#define BMA_REG_INT_STATUS_0   0x12
#define BMA_REG_INT_STATUS_1   0x13
#define BMA_REG_ACC_DATA_0     0x18
#define BMA_REG_ACC_DATA_5     0x1D
#define BMA_REG_TEMP_DATA      0x1E

#define BMA_CHIP_ID            0xC4
#define BMA_HEALTH_OK          0x0F

/* INT_STATUS_INT1_0 */
#define BMA_INT0_ACC_DRDY      0x01
#define BMA_INT0_STAP          0x80
/* INT_STATUS_INT1_1 */
#define BMA_INT1_DTAP          0x01
#define BMA_INT1_TTAP          0x02

struct HaloBma580State {
    I2CSlave parent_obj;

    uint8_t reg[256];
    uint8_t ptr;
    bool addr_got;

    int32_t accel[3]; /* raw LSB counts, x/y/z */
    int32_t temp;     /* raw TEMP_DATA byte */

    uint8_t int_status[2]; /* pending INT_STATUS_INT1_0/1 bits */
    qemu_irq int1;
};

static void halo_bma580_update_int(HaloBma580State *s)
{
    qemu_set_irq(s->int1, !!(s->int_status[0] | s->int_status[1]));
}

static int halo_bma580_event(I2CSlave *i2c, enum i2c_event event)
{
    HaloBma580State *s = HALO_BMA580(i2c);

    if (event == I2C_START_SEND) {
        s->addr_got = false;
    }
    return 0;
}

static int halo_bma580_send(I2CSlave *i2c, uint8_t data)
{
    HaloBma580State *s = HALO_BMA580(i2c);

    if (!s->addr_got) {
        s->ptr = data;
        s->addr_got = true;
        return 0;
    }

    switch (s->ptr) {
    case BMA_REG_INT_STATUS_0:
    case BMA_REG_INT_STATUS_1:
        /* write-1-to-clear */
        s->int_status[s->ptr - BMA_REG_INT_STATUS_0] &= ~data;
        halo_bma580_update_int(s);
        break;
    default:
        s->reg[s->ptr] = data;
        break;
    }
    s->ptr++;
    return 0;
}

static uint8_t halo_bma580_recv(I2CSlave *i2c)
{
    HaloBma580State *s = HALO_BMA580(i2c);
    uint8_t addr = s->ptr++;

    if (addr >= BMA_REG_ACC_DATA_0 && addr <= BMA_REG_ACC_DATA_5) {
        unsigned idx = addr - BMA_REG_ACC_DATA_0;
        uint16_t v = (uint16_t)(int16_t)s->accel[idx / 2];

        return (idx & 1) ? (v >> 8) : (v & 0xff);
    }

    switch (addr) {
    case BMA_REG_TEMP_DATA:
        return (uint8_t)s->temp;
    case BMA_REG_INT_STATUS_0:
    case BMA_REG_INT_STATUS_1:
        return s->int_status[addr - BMA_REG_INT_STATUS_0];
    default:
        return s->reg[addr];
    }
}

static void halo_bma580_get_int32(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    int64_t value = *(int32_t *)opaque;

    visit_type_int(v, name, &value, errp);
}

static void halo_bma580_set_accel(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < INT16_MIN || value > INT16_MAX) {
        error_setg(errp, "%s must be an int16 LSB count (%d..%d)",
                   name, INT16_MIN, INT16_MAX);
        return;
    }
    *(int32_t *)opaque = value;
}

static void halo_bma580_set_temp(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    HaloBma580State *s = HALO_BMA580(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 0xff) {
        error_setg(errp, "%s must be a raw register byte (0..255)", name);
        return;
    }
    s->temp = value;
}

static void halo_bma580_set_tap(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    HaloBma580State *s = HALO_BMA580(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    switch (value) {
    case 1:
        s->int_status[0] |= BMA_INT0_STAP;
        break;
    case 2:
        s->int_status[1] |= BMA_INT1_DTAP;
        break;
    case 3:
        s->int_status[1] |= BMA_INT1_TTAP;
        break;
    default:
        error_setg(errp, "tap must be 1 (single), 2 (double) or 3 (triple)");
        return;
    }
    halo_bma580_update_int(s);
}

static void halo_bma580_set_drdy(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    HaloBma580State *s = HALO_BMA580(obj);
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (value) {
        s->int_status[0] |= BMA_INT0_ACC_DRDY;
    } else {
        s->int_status[0] &= ~BMA_INT0_ACC_DRDY;
    }
    halo_bma580_update_int(s);
}

static void halo_bma580_reset(DeviceState *dev)
{
    HaloBma580State *s = HALO_BMA580(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[BMA_REG_CHIP_ID] = BMA_CHIP_ID;
    s->reg[BMA_REG_HEALTH_STATUS] = BMA_HEALTH_OK;
    s->ptr = 0;
    s->addr_got = false;
    s->int_status[0] = 0;
    s->int_status[1] = 0;
    halo_bma580_update_int(s);
    /* The injected sample survives reset, like the other runtime
     * controls: the control socket may have set it before a reboot. */
}

static void halo_bma580_init(Object *obj)
{
    HaloBma580State *s = HALO_BMA580(obj);
    static const char *const axis[3] = { "accel-x", "accel-y", "accel-z" };

    for (unsigned i = 0; i < 3; i++) {
        object_property_add(obj, axis[i], "int", halo_bma580_get_int32,
                            halo_bma580_set_accel, NULL, &s->accel[i]);
    }
    object_property_add(obj, "temp", "int", halo_bma580_get_int32,
                        halo_bma580_set_temp, NULL, &s->temp);
    object_property_add(obj, "tap", "int", NULL, halo_bma580_set_tap,
                        NULL, NULL);
    object_property_add(obj, "data-ready", "bool", NULL,
                        halo_bma580_set_drdy, NULL, NULL);

    qdev_init_gpio_out_named(DEVICE(obj), &s->int1, "int1", 1);
}

static void halo_bma580_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = halo_bma580_event;
    k->send = halo_bma580_send;
    k->recv = halo_bma580_recv;
    dc->desc = "BMA580 accelerometer (halo)";
    device_class_set_legacy_reset(dc, halo_bma580_reset);
}

static const TypeInfo halo_bma580_types[] = {
    {
        .name = TYPE_HALO_BMA580,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HaloBma580State),
        .instance_init = halo_bma580_init,
        .class_init = halo_bma580_class_init,
    },
};

DEFINE_TYPES(halo_bma580_types)
