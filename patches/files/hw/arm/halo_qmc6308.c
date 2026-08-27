/*
 * Halo — QMC6308 magnetometer model (I2C0 @ 0x2C).
 *
 * A register file with the two things a plain ack-everything target
 * cannot provide (see halo_i2c_regfile.c, which is not enough here):
 *
 *  - CHIPID (0x00) reads 0x80. qmc6308_init() rejects anything else
 *    with "Invalid chip ID" (qmc6308.c:442-449), so a zeroed register
 *    file never probes.
 *  - STATUS (0x09) bit0 (DRDY) is always set. qmc6308_sample_fetch()
 *    spins on it and gives up with -ETIMEDOUT after 100 ms
 *    (qmc6308.c:271-292).
 *
 * DATA_OUT_{X,Y,Z}_{L,H} (0x01-0x06) are served from the injected
 * sample rather than from storage: little-endian int16 per axis, the
 * layout the driver reassembles at qmc6308.c:300-302. Everything else
 * behaves like a normal register file, which covers the driver's
 * read-modify-write of CONF_0/CONF_1/CTRL.
 *
 * Injection: the "magn-x/y/z" properties carry raw LSB counts, driven
 * from the machine's like-named properties (ticket 0037).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"

#define TYPE_HALO_QMC6308 "halo-qmc6308"
OBJECT_DECLARE_SIMPLE_TYPE(HaloQmc6308State, HALO_QMC6308)

#define QMC_REG_CHIPID     0x00
#define QMC_REG_DATA_X_L   0x01
#define QMC_REG_DATA_Z_H   0x06
#define QMC_REG_STATUS     0x09

#define QMC_CHIP_ID        0x80
#define QMC_STATUS_DRDY    0x01

struct HaloQmc6308State {
    I2CSlave parent_obj;

    uint8_t reg[256];
    uint8_t ptr;
    bool addr_got;

    int32_t magn[3]; /* raw LSB counts, x/y/z */
};

static int halo_qmc6308_event(I2CSlave *i2c, enum i2c_event event)
{
    HaloQmc6308State *s = HALO_QMC6308(i2c);

    if (event == I2C_START_SEND) {
        s->addr_got = false;
    }
    return 0;
}

static int halo_qmc6308_send(I2CSlave *i2c, uint8_t data)
{
    HaloQmc6308State *s = HALO_QMC6308(i2c);

    if (!s->addr_got) {
        s->ptr = data;
        s->addr_got = true;
    } else {
        s->reg[s->ptr++] = data;
    }
    return 0;
}

static uint8_t halo_qmc6308_recv(I2CSlave *i2c)
{
    HaloQmc6308State *s = HALO_QMC6308(i2c);
    uint8_t addr = s->ptr++;

    if (addr >= QMC_REG_DATA_X_L && addr <= QMC_REG_DATA_Z_H) {
        unsigned idx = addr - QMC_REG_DATA_X_L;
        uint16_t v = (uint16_t)(int16_t)s->magn[idx / 2];

        return (idx & 1) ? (v >> 8) : (v & 0xff);
    }
    if (addr == QMC_REG_STATUS) {
        return s->reg[addr] | QMC_STATUS_DRDY;
    }
    return s->reg[addr];
}

static void halo_qmc6308_get_magn(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    HaloQmc6308State *s = HALO_QMC6308(obj);
    int64_t value = s->magn[(uintptr_t)opaque];

    visit_type_int(v, name, &value, errp);
}

static void halo_qmc6308_set_magn(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    HaloQmc6308State *s = HALO_QMC6308(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < INT16_MIN || value > INT16_MAX) {
        error_setg(errp, "%s must be an int16 LSB count (%d..%d)",
                   name, INT16_MIN, INT16_MAX);
        return;
    }
    s->magn[(uintptr_t)opaque] = value;
}

static void halo_qmc6308_reset(DeviceState *dev)
{
    HaloQmc6308State *s = HALO_QMC6308(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[QMC_REG_CHIPID] = QMC_CHIP_ID;
    s->ptr = 0;
    s->addr_got = false;
    /* The injected field survives reset: the control socket may have set
     * a heading before the guest rebooted. */
}

static void halo_qmc6308_init(Object *obj)
{
    static const char *const axis[3] = { "magn-x", "magn-y", "magn-z" };

    for (uintptr_t i = 0; i < 3; i++) {
        object_property_add(obj, axis[i], "int",
                            halo_qmc6308_get_magn, halo_qmc6308_set_magn,
                            NULL, (void *)i);
    }
}

static void halo_qmc6308_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = halo_qmc6308_event;
    k->send = halo_qmc6308_send;
    k->recv = halo_qmc6308_recv;
    dc->desc = "QMC6308 magnetometer (halo)";
    device_class_set_legacy_reset(dc, halo_qmc6308_reset);
}

static const TypeInfo halo_qmc6308_types[] = {
    {
        .name = TYPE_HALO_QMC6308,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HaloQmc6308State),
        .instance_init = halo_qmc6308_init,
        .class_init = halo_qmc6308_class_init,
    },
};

DEFINE_TYPES(halo_qmc6308_types)
