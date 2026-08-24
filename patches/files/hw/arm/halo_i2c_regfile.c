/*
 * Halo — generic ack-everything I2C register-file target.
 *
 * Stands in for the display-path I2C1 devices whose only contract with
 * the firmware is "the transfer succeeds and reads return the last
 * written value":
 *
 *  - vga020 panel @ 0x54: 16-bit big-endian register address followed
 *    by data bytes (display_vga020.c writes reg 0x6C00 at hw-init and
 *    on the backlight/standby paths; its read path is
 *    i2c_write_read_dt with the same 2-byte address).
 *  - TPS65132 display PMIC @ 0x3E: standard 8-bit register device
 *    (regulator_tps65132.c reads VPOS/VNEG/CFG back and only writes
 *    when the value differs — a zeroed register file makes it program
 *    everything on first enable, like silicon fresh out of reset).
 *
 * The address-byte count is a property ("addr-bytes", 1 or 2).  A
 * write transfer's first addr-bytes bytes load the register pointer
 * (MSB first); subsequent bytes store to consecutive registers.  Reads
 * return consecutive registers from the current pointer.  Nothing ever
 * NAKs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_HALO_I2C_REGFILE "halo-i2c-regfile"
OBJECT_DECLARE_SIMPLE_TYPE(HaloI2cRegfileState, HALO_I2C_REGFILE)

struct HaloI2cRegfileState {
    I2CSlave parent_obj;

    uint32_t addr_bytes; /* property: register-address bytes, 1 or 2 */

    uint8_t reg[0x10000];
    uint16_t ptr;
    uint8_t addr_got;
};

static int halo_i2c_regfile_event(I2CSlave *i2c, enum i2c_event event)
{
    HaloI2cRegfileState *s = HALO_I2C_REGFILE(i2c);

    if (event == I2C_START_SEND) {
        s->addr_got = 0;
    }
    return 0;
}

static int halo_i2c_regfile_send(I2CSlave *i2c, uint8_t data)
{
    HaloI2cRegfileState *s = HALO_I2C_REGFILE(i2c);

    if (s->addr_got < s->addr_bytes) {
        s->ptr = (s->ptr << 8) | data;
        if (s->addr_bytes == 1) {
            s->ptr = data;
        }
        s->addr_got++;
    } else {
        s->reg[s->ptr++] = data;
    }
    return 0;
}

static uint8_t halo_i2c_regfile_recv(I2CSlave *i2c)
{
    HaloI2cRegfileState *s = HALO_I2C_REGFILE(i2c);

    return s->reg[s->ptr++];
}

static void halo_i2c_regfile_reset(DeviceState *dev)
{
    HaloI2cRegfileState *s = HALO_I2C_REGFILE(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->ptr = 0;
    s->addr_got = 0;
}

static const Property halo_i2c_regfile_properties[] = {
    DEFINE_PROP_UINT32("addr-bytes", HaloI2cRegfileState, addr_bytes, 1),
};

static void halo_i2c_regfile_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = halo_i2c_regfile_event;
    k->send = halo_i2c_regfile_send;
    k->recv = halo_i2c_regfile_recv;
    dc->desc = "ack-everything I2C register file (halo)";
    device_class_set_legacy_reset(dc, halo_i2c_regfile_reset);
    device_class_set_props(dc, halo_i2c_regfile_properties);
}

static const TypeInfo halo_i2c_regfile_types[] = {
    {
        .name = TYPE_HALO_I2C_REGFILE,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HaloI2cRegfileState),
        .class_init = halo_i2c_regfile_class_init,
    },
};

DEFINE_TYPES(halo_i2c_regfile_types)
