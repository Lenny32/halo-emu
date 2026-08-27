/*
 * Halo — PixArt PAG7982 camera sensor model (I2C1 @ 0x40).
 *
 * The sensor's only contract with the firmware is over I2C: the driver
 * (alif/drivers/video/pag7982.c) never reads pixels back through it —
 * the image travels over the parallel pixel bus into the LPCAM
 * controller, which is where halo_lpcam.c synthesises it.  So this
 * model is a bank-switched register file with the two values the
 * driver actually gates on:
 *
 *  - PART_ID_L/H (bank 0, 0x00/0x01) read 0x82/0x79.
 *    pag7982_check_connection() selects bank 0, reads both and returns
 *    -ENODEV on a mismatch (pag7982.c:181-201), which aborts
 *    pag7982_hw_init() and with it the whole camera resume.  They are
 *    read-only: the init sequence writes 0x00/0x01 in *other* banks
 *    (bank 2 has {0x02,0xBF}, bank 4 has {0x00,0x01}), so a
 *    bank-agnostic register file would clobber the ID.
 *  - R_TRG_EN (bank 0, 0x30) is the streaming trigger:
 *    pag7982_stream_start() writes 1, pag7982_stream_stop() writes 0
 *    (pag7982.c:497-524).  Exposed as the read-only "streaming"
 *    property, plus a "triggers" count of its 0 -> 1 transitions —
 *    streaming is only true *during* a capture, so the count is what a
 *    test can assert without racing the guest.
 *
 * Everything else is plain storage, which covers the ~160-entry
 * default_regs sequence, the frame-time programming and the
 * read-modify-write of R_FLIP on the flip controls.
 *
 * BANK_SEL (0xEF) selects the bank.  The sequence also writes the
 * magic 0xA5 unlock value there, which is not a bank at all, so any
 * selector outside the five documented banks lands in one shared
 * scratch bank.
 *
 * Not modelled: reset-gpios (gpio1.4) and the cam_1v8 rail.  The
 * driver drives both as plain outputs and never reads anything back
 * that depends on them, and halo_gpio.c has no output lines to
 * observe, so a reset pulse is invisible here — the register file
 * keeps its contents across one.  The consequence is benign: the init
 * sequence is idempotent and re-runs in full on every resume.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_HALO_PAG7982 "halo-pag7982"
OBJECT_DECLARE_SIMPLE_TYPE(HaloPag7982State, HALO_PAG7982)

#define PAG_REG_PART_ID_L  0x00
#define PAG_REG_PART_ID_H  0x01
#define PAG_REG_TRG_EN     0x30
#define PAG_REG_BANK_SEL   0xEF

#define PAG_ID_L           0x82
#define PAG_ID_H           0x79

/*
 * Banks 0..4 are the ones pag7982.h documents; index 5 absorbs every
 * other selector (the 0xA5 unlock write).
 */
#define PAG_NUM_BANKS      6
#define PAG_SCRATCH_BANK   (PAG_NUM_BANKS - 1)

struct HaloPag7982State {
    I2CSlave parent_obj;

    uint8_t reg[PAG_NUM_BANKS][256];
    uint8_t bank;      /* raw BANK_SEL value as written */
    uint8_t ptr;
    bool addr_got;
    uint32_t triggers; /* R_TRG_EN 0 -> 1 transitions since reset */
};

static uint8_t *halo_pag7982_bank(HaloPag7982State *s)
{
    return s->reg[s->bank < PAG_NUM_BANKS ? s->bank : PAG_SCRATCH_BANK];
}

static int halo_pag7982_event(I2CSlave *i2c, enum i2c_event event)
{
    HaloPag7982State *s = HALO_PAG7982(i2c);

    if (event == I2C_START_SEND) {
        s->addr_got = false;
    }
    return 0;
}

static int halo_pag7982_send(I2CSlave *i2c, uint8_t data)
{
    HaloPag7982State *s = HALO_PAG7982(i2c);

    if (!s->addr_got) {
        s->ptr = data;
        s->addr_got = true;
        return 0;
    }

    if (s->ptr == PAG_REG_BANK_SEL) {
        s->bank = data;
    } else if (s->bank == 0 && (s->ptr == PAG_REG_PART_ID_L ||
                                s->ptr == PAG_REG_PART_ID_H)) {
        /* read-only part ID */
    } else {
        if (s->bank == 0 && s->ptr == PAG_REG_TRG_EN &&
            (data & 0x01) && !(s->reg[0][PAG_REG_TRG_EN] & 0x01)) {
            s->triggers++;
        }
        halo_pag7982_bank(s)[s->ptr] = data;
    }
    s->ptr++;
    return 0;
}

static uint8_t halo_pag7982_recv(I2CSlave *i2c)
{
    HaloPag7982State *s = HALO_PAG7982(i2c);
    uint8_t addr = s->ptr++;

    if (addr == PAG_REG_BANK_SEL) {
        return s->bank;
    }
    if (s->bank == 0) {
        switch (addr) {
        case PAG_REG_PART_ID_L:
            return PAG_ID_L;
        case PAG_REG_PART_ID_H:
            return PAG_ID_H;
        default:
            break;
        }
    }
    return halo_pag7982_bank(s)[addr];
}

static bool halo_pag7982_get_streaming(Object *obj, Error **errp)
{
    HaloPag7982State *s = HALO_PAG7982(obj);

    return !!(s->reg[0][PAG_REG_TRG_EN] & 0x01);
}

static void halo_pag7982_reset(DeviceState *dev)
{
    HaloPag7982State *s = HALO_PAG7982(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->bank = 0;
    s->ptr = 0;
    s->addr_got = false;
    s->triggers = 0;
}

static void halo_pag7982_init(Object *obj)
{
    HaloPag7982State *s = HALO_PAG7982(obj);

    object_property_add_bool(obj, "streaming",
                             halo_pag7982_get_streaming, NULL);
    object_property_add_uint32_ptr(obj, "triggers", &s->triggers,
                                   OBJ_PROP_FLAG_READ);
}

static void halo_pag7982_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = halo_pag7982_event;
    k->send = halo_pag7982_send;
    k->recv = halo_pag7982_recv;
    dc->desc = "PAG7982 camera sensor (halo)";
    device_class_set_legacy_reset(dc, halo_pag7982_reset);
}

static const TypeInfo halo_pag7982_types[] = {
    {
        .name = TYPE_HALO_PAG7982,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(HaloPag7982State),
        .instance_init = halo_pag7982_init,
        .class_init = halo_pag7982_class_init,
    },
};

DEFINE_TYPES(halo_pag7982_types)
