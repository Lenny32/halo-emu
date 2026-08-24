/*
 * Halo — Alif Balletto B1 Secure Enclave fake behind the MHUv2 mailbox
 * pair (sender @ 0x40050000 / IRQ 38, receiver @ 0x40040000 / IRQ 37,
 * channel 0 only).
 *
 * The firmware side is Zephyr's ipm_arm_mhuv2.c driver plus Alif's
 * se_services library: a request is a 32-bit "global" pointer (DTCM
 * alias 0x58800000+) written to the sender's channel-0 CH_SET; the SE
 * writes response fields back into that struct in place and signals
 * completion twice — the sender's channel interrupt (message consumed,
 * CH_ST back to 0) and the receiver's channel-0 CH_ST going nonzero
 * (response delivered).  The library memsets the request struct before
 * every call and treats resp_error_code == 0 as success, so most
 * services are handled by acknowledging and writing nothing back
 * ("ack-and-zero").  Real payloads only where the firmware consumes
 * them: GET_RND (host RNG), the SE version banner, and the TOC version.
 *
 * Register layout per ipm_arm_mhuv2.h: 0x20-byte channel slots
 * (sender CH_ST@0x00 CH_SET@0x0C CH_INT_ST@0x10 CH_INT_CLR@0x14
 * CH_INT_EN@0x18; receiver CH_ST@0x00 CH_ST_MSK@0x04 CH_CLR@0x08),
 * block registers at +0xF80.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/guest-random.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "qom/object.h"

#define TYPE_HALO_SE "halo-se"
OBJECT_DECLARE_SIMPLE_TYPE(HaloSEState, HALO_SE)

/* Block registers (both directions) */
#define R_MHU_CFG        0xF80
#define R_RESP_CFG       0xF84 /* sender only */
#define R_ACCESS_REQUEST 0xF88 /* sender only */
#define R_ACCESS_READY   0xF8C /* sender only */
#define R_INT_ST         0xF90
#define R_INT_CLR        0xF94
#define R_INT_EN         0xF98
#define R_CH_INT_ST0     0xFA0
#define R_IIDR           0xFC8
#define R_AIDR           0xFCC

/* Channel-0 slot */
#define R_CH_ST          0x00
#define R_CH_ST_MSK      0x04 /* receiver */
#define R_CH_CLR         0x08 /* receiver */
#define R_CH_SET         0x0C /* sender */
#define R_CH_INT_ST      0x10 /* sender */
#define R_CH_INT_CLR     0x14 /* sender */
#define R_CH_INT_EN      0x18 /* sender */

#define CHCOMB_INTR      0x4

#define MHU_IIDR         0x0760043B
#define MHU_AIDR         0x00000011

/* Service IDs (services_lib_ids.h) */
#define SE_SVC_HEARTBEAT           0
#define SE_SVC_FW_VERSION          103
#define SE_SVC_GET_TOC_VERSION     200
#define SE_SVC_GET_RND             400
#define SE_SVC_BOOT_RESET_CPU      503
#define SE_SVC_BOOT_RESET_SOC      504

/* get_rnd_svc_t: header @0, u32 send_rnd_length @8, u8 resp_rnd[256] @12 */
#define SE_RND_MAX_LENGTH          256
/* get_se_revision_t: header @0, u32 resp_len @8, u8 resp[80] @12 */
#define SE_VERSION_RESPONSE_LENGTH 80
#define SE_VERSION_BANNER          "SE-EMU v0.50.9 (halo QEMU fake)"
/* get_toc_version_svc_t: header @0, u32 resp_version @8 */
#define SE_TOC_VERSION             0x01660000

struct HaloSEState {
    SysBusDevice parent_obj;

    MemoryRegion recv_iomem; /* mmio[0] @ 0x40040000 */
    MemoryRegion send_iomem; /* mmio[1] @ 0x40050000 */
    qemu_irq recv_irq;       /* IRQ 37 */
    qemu_irq send_irq;       /* IRQ 38 */

    /* sender-side state, channel 0 only (MHU_CFG reads 1 channel) */
    uint32_t s_resp_cfg;
    uint32_t s_access_req;
    uint32_t s_int_en;
    uint32_t s_ch_int_st;
    uint32_t s_ch_int_en;

    /* receiver-side state */
    uint32_t r_int_en;
    uint32_t r_ch_st;
};

static void halo_se_update_irqs(HaloSEState *s)
{
    qemu_set_irq(s->send_irq,
                 (s->s_int_en & CHCOMB_INTR) && (s->s_ch_int_st & 1));
    qemu_set_irq(s->recv_irq,
                 (s->r_int_en & CHCOMB_INTR) && s->r_ch_st);
}

static bool halo_se_mem_read(uint32_t addr, void *buf, uint32_t len)
{
    return address_space_read(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static bool halo_se_mem_write(uint32_t addr, const void *buf, uint32_t len)
{
    return address_space_write(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

/*
 * Handle one service request.  `req` is the global-alias address of the
 * request struct (service_header_t {u16 id; u16 flags; u16 error; u16 pad}
 * followed by per-service fields).  The struct arrives zeroed, so success
 * (resp_error_code == 0) needs no write-back — only fill real payloads.
 */
static void halo_se_service(HaloSEState *s, uint32_t req)
{
    uint16_t id;

    if (!halo_se_mem_read(req, &id, sizeof(id))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "halo-se: request pointer 0x%08x unreadable\n", req);
        return;
    }
    id = le16_to_cpu(id);

    switch (id) {
    case SE_SVC_GET_RND: {
        uint32_t len;
        uint8_t rnd[SE_RND_MAX_LENGTH];

        if (!halo_se_mem_read(req + 8, &len, sizeof(len))) {
            return;
        }
        len = MIN(le32_to_cpu(len), (uint32_t)SE_RND_MAX_LENGTH);
        qemu_guest_getrandom_nofail(rnd, len);
        halo_se_mem_write(req + 12, rnd, len);
        break;
    }
    case SE_SVC_FW_VERSION: {
        char banner[SE_VERSION_RESPONSE_LENGTH] = SE_VERSION_BANNER;
        uint32_t len = cpu_to_le32(sizeof(SE_VERSION_BANNER));

        halo_se_mem_write(req + 8, &len, sizeof(len));
        halo_se_mem_write(req + 12, banner, sizeof(SE_VERSION_BANNER));
        break;
    }
    case SE_SVC_GET_TOC_VERSION: {
        uint32_t version = cpu_to_le32(SE_TOC_VERSION);

        halo_se_mem_write(req + 8, &version, sizeof(version));
        break;
    }
    case SE_SVC_HEARTBEAT:
        break;
    case SE_SVC_BOOT_RESET_CPU:
    case SE_SVC_BOOT_RESET_SOC: {
        /* sys_reboot() on the Balletto is an SE service call (the SoC has
         * no self-reset path from the RTSS): reset the whole machine in
         * place.  MRAM is a mapped host file, so /lfs survives — exactly
         * the hardware's cold reboot (ticket 0030, control code 0x02). */
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    }
    default:
        /* ack-and-zero: the zeroed struct already reads as success */
        qemu_log_mask(LOG_UNIMP,
                      "halo-se: service %u acked with zero response\n", id);
        break;
    }
}

/*
 * Channel-0 CH_SET on the sender: consume the message immediately
 * (sender CH_ST stays 0), latch the sender channel interrupt if armed,
 * and deliver the response by raising the receiver's channel-0 CH_ST
 * (the receiver callback ignores the value; echo the request pointer,
 * which is conveniently nonzero).
 */
static void halo_se_ch_set(HaloSEState *s, uint32_t value)
{
    halo_se_service(s, value);

    if (s->s_ch_int_en & 1) {
        s->s_ch_int_st = 1;
    }
    s->r_ch_st = value ? value : 1;
    halo_se_update_irqs(s);
}

static uint64_t halo_se_send_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloSEState *s = opaque;

    switch (offset) {
    case R_CH_ST:
        return 0; /* messages are consumed instantly */
    case R_CH_INT_ST:
        return s->s_ch_int_st;
    case R_CH_INT_EN:
        return s->s_ch_int_en;
    case R_MHU_CFG:
        return 1; /* one channel */
    case R_RESP_CFG:
        return s->s_resp_cfg;
    case R_ACCESS_REQUEST:
        return s->s_access_req;
    case R_ACCESS_READY:
        return s->s_access_req & 1; /* SE is always ready when asked */
    case R_INT_ST:
        return (s->s_ch_int_st & 1) ? CHCOMB_INTR : 0;
    case R_INT_EN:
        return s->s_int_en;
    case R_CH_INT_ST0:
        return s->s_ch_int_st & 1;
    case R_IIDR:
        return MHU_IIDR;
    case R_AIDR:
        return MHU_AIDR;
    default:
        return 0;
    }
}

static void halo_se_send_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    HaloSEState *s = opaque;

    switch (offset) {
    case R_CH_SET:
        halo_se_ch_set(s, value);
        break;
    case R_CH_INT_CLR:
        if (value & 1) {
            s->s_ch_int_st = 0;
            halo_se_update_irqs(s);
        }
        break;
    case R_CH_INT_EN:
        s->s_ch_int_en = value & 1;
        break;
    case R_RESP_CFG:
        s->s_resp_cfg = value;
        break;
    case R_ACCESS_REQUEST:
        s->s_access_req = value & 1;
        break;
    case R_INT_CLR:
        break; /* NR2R/R2NR are never raised */
    case R_INT_EN:
        s->s_int_en = value & 7;
        halo_se_update_irqs(s);
        break;
    default:
        break;
    }
}

static uint64_t halo_se_recv_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloSEState *s = opaque;

    switch (offset) {
    case R_CH_ST:
    case R_CH_ST_MSK:
        return s->r_ch_st;
    case R_MHU_CFG:
        return 1; /* one channel */
    case R_INT_ST:
        return s->r_ch_st ? CHCOMB_INTR : 0;
    case R_INT_EN:
        return s->r_int_en;
    case R_CH_INT_ST0:
        return s->r_ch_st ? 1 : 0;
    case R_IIDR:
        return MHU_IIDR;
    case R_AIDR:
        return MHU_AIDR;
    default:
        return 0;
    }
}

static void halo_se_recv_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    HaloSEState *s = opaque;

    switch (offset) {
    case R_CH_CLR:
        s->r_ch_st &= ~value;
        halo_se_update_irqs(s);
        break;
    case R_INT_CLR:
        break;
    case R_INT_EN:
        s->r_int_en = value & 7;
        halo_se_update_irqs(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps halo_se_send_ops = {
    .read = halo_se_send_read,
    .write = halo_se_send_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps halo_se_recv_ops = {
    .read = halo_se_recv_read,
    .write = halo_se_recv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_se_reset(DeviceState *dev)
{
    HaloSEState *s = HALO_SE(dev);

    s->s_resp_cfg = 0;
    s->s_access_req = 0;
    s->s_int_en = 0;
    s->s_ch_int_st = 0;
    s->s_ch_int_en = 0;
    s->r_int_en = 0;
    s->r_ch_st = 0;
    halo_se_update_irqs(s);
}

static void halo_se_init(Object *obj)
{
    HaloSEState *s = HALO_SE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->recv_iomem, obj, &halo_se_recv_ops, s,
                          "halo-se.recv", 0x1000);
    sysbus_init_mmio(sbd, &s->recv_iomem);
    memory_region_init_io(&s->send_iomem, obj, &halo_se_send_ops, s,
                          "halo-se.send", 0x1000);
    sysbus_init_mmio(sbd, &s->send_iomem);
    sysbus_init_irq(sbd, &s->recv_irq);
    sysbus_init_irq(sbd, &s->send_irq);
}

static void halo_se_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, halo_se_reset);
}

static const TypeInfo halo_se_types[] = {
    {
        .name = TYPE_HALO_SE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloSEState),
        .instance_init = halo_se_init,
        .class_init = halo_se_class_init,
    },
};

DEFINE_TYPES(halo_se_types)
