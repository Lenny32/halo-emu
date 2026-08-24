/*
 * Halo — DesignWare MIPI DSI host @ 0x49032000 (snps,designware-dsi,
 * IRQ 343): happy-path fake so the panel bring-up succeeds.
 *
 * The real link (D-PHY lanes, DCS packets to the panel) has no
 * observable effect in the emulator — the CDC200 model scans the
 * framebuffer out directly — so this device only has to satisfy what
 * the firmware drivers read back (dsi_dw.c, dphy_dw.c):
 *
 *  - DSI_PHY_STATUS @ 0xB0: dphy_dw_master_setup() polls (bounded,
 *    1e6 iterations) for PHY_LOCK and then for the stop-state bits of
 *    the clock lane + both data lanes.  Reads return all of them,
 *    permanently: 0x95 = PHY_LOCK | STOPSTATECLKLANE | STOPSTATE0LANE
 *    | STOPSTATE1LANE.  Without this the boot-logo splash is skipped
 *    (vga020_hw_init fails -EIO, non-fatal).
 *  - DSI_CMD_PKT_STATUS @ 0x74: the DCS/generic packet paths wait for
 *    GEN_CMD_EMPTY | GEN_PLD_W_EMPTY (writes complete instantly) and
 *    must not see GEN_RD_CMD_BUSY.  0x15 also keeps GEN_PLD_R_EMPTY
 *    set — a generic read returns no payload and fails cleanly.
 *  - DSI_GEN_HDR/GEN_PLD_DATA: packets to the panel disappear; payload
 *    reads return 0.
 *  - DSI_INT_ST0/ST1: read-to-clear error status, always 0.
 *  - Everything else is plain storage — the driver read-modify-writes
 *    its config registers (VID_MODE_CFG, PCKHDL_CFG, LPCLK_CTRL, the
 *    PHY_TST_CTRL0/1 test-interface used to program the D-PHY PLL) and
 *    only needs its own values back.  The test-interface reads used in
 *    the PLL setup RMW sequences see TESTDOUT=0, which is fine — the
 *    values are never checked, only rewritten.
 *
 * The D-PHY control registers themselves (DPHY_PLL_CTRL0 etc.) live in
 * the EXPMST block @ 0x4903F000, which the machine already backs with
 * register-file RAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_HALO_DSI "halo-dsi"
OBJECT_DECLARE_SIMPLE_TYPE(HaloDsiState, HALO_DSI)

#define R_GEN_HDR         0x6C
#define R_GEN_PLD_DATA    0x70
#define R_CMD_PKT_STATUS  0x74
#define R_PHY_STATUS      0xB0
#define R_INT_ST0         0xBC
#define R_INT_ST1         0xC0

/* GEN_CMD_EMPTY | GEN_PLD_W_EMPTY | GEN_PLD_R_EMPTY */
#define CMD_PKT_STATUS_IDLE 0x15
/* PHY_LOCK | STOPSTATECLKLANE | STOPSTATE0LANE | STOPSTATE1LANE */
#define PHY_STATUS_UP       0x95

#define DSI_NUM_REGS (0x1000 / 4)

struct HaloDsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq; /* wired, never raised */

    uint32_t reg[DSI_NUM_REGS];
};

static uint64_t halo_dsi_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloDsiState *s = opaque;

    switch (offset) {
    case R_PHY_STATUS:
        return PHY_STATUS_UP;
    case R_CMD_PKT_STATUS:
        return CMD_PKT_STATUS_IDLE;
    case R_GEN_PLD_DATA:
    case R_INT_ST0:
    case R_INT_ST1:
        return 0;
    default:
        return s->reg[offset / 4];
    }
}

static void halo_dsi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloDsiState *s = opaque;

    switch (offset) {
    case R_GEN_HDR:
    case R_GEN_PLD_DATA:
        /* Packet to the panel: swallowed */
        return;
    default:
        s->reg[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps halo_dsi_ops = {
    .read = halo_dsi_read,
    .write = halo_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_dsi_reset(DeviceState *dev)
{
    HaloDsiState *s = HALO_DSI(dev);

    memset(s->reg, 0, sizeof(s->reg));
}

static void halo_dsi_init(Object *obj)
{
    HaloDsiState *s = HALO_DSI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_dsi_ops, s,
                          TYPE_HALO_DSI, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void halo_dsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "DesignWare MIPI DSI host fake (halo)";
    device_class_set_legacy_reset(dc, halo_dsi_reset);
}

static const TypeInfo halo_dsi_types[] = {
    {
        .name = TYPE_HALO_DSI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloDsiState),
        .instance_init = halo_dsi_init,
        .class_init = halo_dsi_class_init,
    },
};

DEFINE_TYPES(halo_dsi_types)
