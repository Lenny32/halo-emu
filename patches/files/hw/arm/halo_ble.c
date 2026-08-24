/*
 * Halo — BLE doorbell device for the synthetic ROM stub (ticket 0028).
 *
 * The firmware's BLE host stack is a synthetic ROM stub (rom-stub/ in the
 * emulator repo) loaded into the ROM window.  This device is its transport
 * to the outside world:
 *
 *  - region 0 (0x4904E000): doorbell page.  The stub writes G2H_KICK after
 *    appending frames to the guest->host ring; the device forwards the
 *    bytes to its chardev (halo-emu exposes it as TCP, ticket 0030 builds
 *    the Lua REPL bridge on it).  TRAP_LR/TRAP_IDX report calls into
 *    unimplemented pinned ROM symbols; with a symbol table (`symfile`
 *    property, produced by the rom-stub build) they are logged by name.
 *
 *  - region 1 (0x48001000): the UTIMER0 channel page used by the
 *    firmware's BLE sync-timer driver (modules/hal/alif/ble/plf/
 *    sync_timer.c).  A free-running counter plus the CHAN_INTERRUPT
 *    register; the device raises the capture-A IRQ (NVIC 377) to signal
 *    "host->guest frames pending" — the firmware ISR clears CHAN_INTERRUPT
 *    (write-1-to-clear) and invokes the capture callback the ROM stub
 *    registered, which schedules the BLE task to drain the ring.
 *
 *  - chardev input: framed byte stream ({op, flags, len16, payload}); each
 *    complete frame is copied into the host->guest ring in the ROM window
 *    and the IRQ is raised.
 *
 * Ring layout and opcodes: halo_rom_ipc.h (kept in sync with
 * rom-stub/src/halo_rom_ipc.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#include "halo_rom_ipc.h"

#define TYPE_HALO_BLE "halo-ble"
OBJECT_DECLARE_SIMPLE_TYPE(HaloBLEState, HALO_BLE)

/* UTIMER channel register offsets (subset the sync-timer driver touches) */
#define UT_CNTR_CTRL 0x80
#define UT_CNTR 0xA0
#define UT_CNTR_PTR 0xA4
#define UT_CAPTURE_A 0xB0
#define UT_CHAN_STATUS 0x114
#define UT_CHAN_INTERRUPT 0x118
#define UT_CHAN_INTERRUPT_MASK 0x11C

#define UT_INT_CAPTURE_A 0x01
#define UT_CLK_HZ 40000000 /* nominal UTIMER clock; only monotonicity matters */

#define HALO_BLE_MAX_FRAME 2048

struct HaloBLEState {
    SysBusDevice parent_obj;

    MemoryRegion doorbell;
    MemoryRegion utimer;
    qemu_irq irq;
    CharFrontend chr;
    char *symfile;

    /* trap report latch + de-dup (log each symbol once) */
    uint32_t trap_lr;
    unsigned long *trap_seen;
    GPtrArray *symnames; /* index -> name, from symfile */

    /* pending IRQ bits of the fake UTIMER channel */
    uint32_t chan_int;

    /* chardev frame reassembly + H2G flow control: when the ring has no
     * room for the next complete frame, it stays parked in rxbuf,
     * chr_can_receive throttles the chardev, and retry_timer polls for
     * space (the guest advances the tail with no MMIO exit to hook). */
    uint8_t rxbuf[4 + HALO_BLE_MAX_FRAME];
    unsigned rxlen;
    bool rx_blocked;
    QEMUTimer retry_timer;
};

/* ------------------------------------------------------------------ */
/* Ring access (guest memory)                                          */
/* ------------------------------------------------------------------ */

static uint32_t ring_ld32(hwaddr ring, hwaddr off)
{
    uint32_t v;

    address_space_read(&address_space_memory, ring + off,
                       MEMTXATTRS_UNSPECIFIED, &v, 4);
    return le32_to_cpu(v);
}

static void ring_st32(hwaddr ring, hwaddr off, uint32_t v)
{
    v = cpu_to_le32(v);
    address_space_write(&address_space_memory, ring + off,
                        MEMTXATTRS_UNSPECIFIED, &v, 4);
}

static bool h2g_push(HaloBLEState *s, const uint8_t *frame, uint32_t len)
{
    uint32_t head = ring_ld32(HALO_BLE_H2G_RING_ADDR, HALO_BLE_RING_OFF_HEAD);
    uint32_t tail = ring_ld32(HALO_BLE_H2G_RING_ADDR, HALO_BLE_RING_OFF_TAIL);

    if (HALO_BLE_RING_DATA - (head - tail) < len) {
        return false; /* no room — caller parks the frame and retries */
    }

    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = frame[i];

        address_space_write(&address_space_memory,
                            HALO_BLE_H2G_RING_ADDR + HALO_BLE_RING_OFF_DATA +
                                ((head + i) & (HALO_BLE_RING_DATA - 1)),
                            MEMTXATTRS_UNSPECIFIED, &b, 1);
    }
    ring_st32(HALO_BLE_H2G_RING_ADDR, HALO_BLE_RING_OFF_HEAD, head + len);

    s->chan_int |= UT_INT_CAPTURE_A;
    qemu_set_irq(s->irq, 1);
    return true;
}

static void g2h_drain(HaloBLEState *s)
{
    uint32_t head = ring_ld32(HALO_BLE_G2H_RING_ADDR, HALO_BLE_RING_OFF_HEAD);
    uint32_t tail = ring_ld32(HALO_BLE_G2H_RING_ADDR, HALO_BLE_RING_OFF_TAIL);
    uint8_t buf[256];

    while (tail != head) {
        uint32_t n = MIN(head - tail, (uint32_t)sizeof(buf));

        for (uint32_t i = 0; i < n; i++) {
            address_space_read(&address_space_memory,
                               HALO_BLE_G2H_RING_ADDR +
                                   HALO_BLE_RING_OFF_DATA +
                                   ((tail + i) & (HALO_BLE_RING_DATA - 1)),
                               MEMTXATTRS_UNSPECIFIED, &buf[i], 1);
        }
        /* Forward even with no client connected: the write just drops. */
        qemu_chr_fe_write_all(&s->chr, buf, n);
        tail += n;
    }
    ring_st32(HALO_BLE_G2H_RING_ADDR, HALO_BLE_RING_OFF_TAIL, tail);
}

/* ------------------------------------------------------------------ */
/* Doorbell MMIO                                                       */
/* ------------------------------------------------------------------ */

static void trap_log(HaloBLEState *s, uint32_t idx, uint32_t lr)
{
    const char *name = "?";

    if (s->symnames && idx < s->symnames->len) {
        name = g_ptr_array_index(s->symnames, idx);
    }
    if (s->trap_seen && idx < 1024 && test_and_set_bit(idx, s->trap_seen)) {
        return; /* already reported */
    }
    warn_report("halo-ble: ROM stub trap: %s (symbol #%u) called from "
                "LR=0x%08x — unimplemented pinned ROM function",
                name, idx, lr);
}

static uint64_t doorbell_read(void *opaque, hwaddr offset, unsigned size)
{
    switch (offset) {
    case HALO_BLE_REG_MAGIC:
        return HALO_ROM_HDR_MAGIC;
    default:
        return 0;
    }
}

static void doorbell_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloBLEState *s = opaque;

    switch (offset) {
    case HALO_BLE_REG_G2H_KICK:
        g2h_drain(s);
        break;
    case HALO_BLE_REG_TRAP_LR:
        s->trap_lr = value;
        break;
    case HALO_BLE_REG_TRAP_IDX:
        trap_log(s, value, s->trap_lr);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps doorbell_ops = {
    .read = doorbell_read,
    .write = doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Fake UTIMER0 channel                                                 */
/* ------------------------------------------------------------------ */

static uint64_t utimer_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloBLEState *s = opaque;

    switch (offset) {
    case UT_CNTR:
    case UT_CAPTURE_A:
        return (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                          (1000000000 / UT_CLK_HZ));
    case UT_CNTR_CTRL:
        return 1; /* counter running */
    case UT_CHAN_INTERRUPT:
        return s->chan_int;
    case UT_CHAN_STATUS:
        return 0;
    default:
        return 0;
    }
}

static void utimer_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    HaloBLEState *s = opaque;

    switch (offset) {
    case UT_CHAN_INTERRUPT: /* write-1-to-clear */
        s->chan_int &= ~(uint32_t)value;
        if (!(s->chan_int & UT_INT_CAPTURE_A)) {
            qemu_set_irq(s->irq, 0);
        }
        break;
    default:
        /* configuration writes: accepted, no effect */
        break;
    }
}

static const MemoryRegionOps utimer_ops = {
    .read = utimer_read,
    .write = utimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Chardev (host side of the bridge)                                   */
/* ------------------------------------------------------------------ */

#define H2G_RETRY_NS (1 * 1000 * 1000) /* 1 ms ring-space poll */

/* Deliver every complete buffered frame; park and poll when the ring is
 * full so client bytes are never dropped (backpressure, ticket 0030). */
static void rx_deliver(HaloBLEState *s)
{
    bool was_blocked = s->rx_blocked;

    for (;;) {
        uint32_t flen;

        if (s->rxlen < 4) {
            break;
        }
        flen = 4 + (s->rxbuf[2] | (s->rxbuf[3] << 8));
        if (flen > sizeof(s->rxbuf)) {
            warn_report("halo-ble: oversized frame from client (%u bytes), "
                        "resetting stream", flen);
            s->rxlen = 0;
            break;
        }
        if (s->rxlen < flen) {
            break;
        }
        if (!h2g_push(s, s->rxbuf, flen)) {
            s->rx_blocked = true;
            timer_mod(&s->retry_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + H2G_RETRY_NS);
            return;
        }
        memmove(s->rxbuf, s->rxbuf + flen, s->rxlen - flen);
        s->rxlen -= flen;
    }

    s->rx_blocked = false;
    if (was_blocked) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static void rx_retry_cb(void *opaque)
{
    rx_deliver(opaque);
}

static int chr_can_receive(void *opaque)
{
    HaloBLEState *s = opaque;

    return s->rx_blocked ? 0 : sizeof(s->rxbuf) - s->rxlen;
}

static void chr_receive(void *opaque, const uint8_t *buf, int size)
{
    HaloBLEState *s = opaque;

    if (size > (int)(sizeof(s->rxbuf) - s->rxlen)) {
        size = sizeof(s->rxbuf) - s->rxlen;
    }
    memcpy(s->rxbuf + s->rxlen, buf, size);
    s->rxlen += size;
    rx_deliver(s);
}

/* ------------------------------------------------------------------ */
/* Device plumbing                                                     */
/* ------------------------------------------------------------------ */

static void halo_ble_reset(DeviceState *dev)
{
    HaloBLEState *s = HALO_BLE(dev);

    s->chan_int = 0;
    s->rxlen = 0;
    s->rx_blocked = false;
    timer_del(&s->retry_timer);
    s->trap_lr = 0;
    if (s->trap_seen) {
        bitmap_zero(s->trap_seen, 1024);
    }
    qemu_set_irq(s->irq, 0);

    /* Own the ring headers: the ROM window RAM behind them is filled with
     * `bx lr` / stub image bytes at machine init. */
    ring_st32(HALO_BLE_H2G_RING_ADDR, HALO_BLE_RING_OFF_HEAD, 0);
    ring_st32(HALO_BLE_H2G_RING_ADDR, HALO_BLE_RING_OFF_TAIL, 0);
    ring_st32(HALO_BLE_G2H_RING_ADDR, HALO_BLE_RING_OFF_HEAD, 0);
    ring_st32(HALO_BLE_G2H_RING_ADDR, HALO_BLE_RING_OFF_TAIL, 0);
}

static void halo_ble_load_symfile(HaloBLEState *s)
{
    g_autofree char *contents = NULL;
    char **lines;

    if (!s->symfile) {
        return;
    }
    if (!g_file_get_contents(s->symfile, &contents, NULL, NULL)) {
        warn_report("halo-ble: cannot read symfile %s — traps will be "
                    "reported by index only", s->symfile);
        return;
    }

    s->symnames = g_ptr_array_new_with_free_func(g_free);
    lines = g_strsplit(contents, "\n", -1);
    for (char **l = lines; *l && **l; l++) {
        /* "<idx> <hex-addr> <name>" in index order */
        char *sp = strrchr(*l, ' ');

        g_ptr_array_add(s->symnames, g_strdup(sp ? sp + 1 : *l));
    }
    g_strfreev(lines);
}

static void halo_ble_realize(DeviceState *dev, Error **errp)
{
    HaloBLEState *s = HALO_BLE(dev);

    memory_region_init_io(&s->doorbell, OBJECT(s), &doorbell_ops, s,
                          "halo-ble.doorbell", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->doorbell);
    memory_region_init_io(&s->utimer, OBJECT(s), &utimer_ops, s,
                          "halo-ble.utimer0", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->utimer);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);

    s->trap_seen = bitmap_new(1024);
    halo_ble_load_symfile(s);

    timer_init_ns(&s->retry_timer, QEMU_CLOCK_VIRTUAL, rx_retry_cb, s);
    qemu_chr_fe_set_handlers(&s->chr, chr_can_receive, chr_receive, NULL,
                             NULL, s, NULL, true);
}

static const Property halo_ble_properties[] = {
    DEFINE_PROP_CHR("chardev", HaloBLEState, chr),
    DEFINE_PROP_STRING("symfile", HaloBLEState, symfile),
};

static void halo_ble_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = halo_ble_realize;
    device_class_set_legacy_reset(dc, halo_ble_reset);
    device_class_set_props(dc, halo_ble_properties);
}

static const TypeInfo halo_ble_types[] = {
    {
        .name = TYPE_HALO_BLE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloBLEState),
        .class_init = halo_ble_class_init,
    },
};

DEFINE_TYPES(halo_ble_types)
