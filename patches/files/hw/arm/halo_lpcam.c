/*
 * Halo — Alif LPCAM (LP-CPI) parallel camera controller @ 0x43003000,
 * IRQ 54 (alif,cam; firmware side zephyr/drivers/video/video_alif.c).
 *
 * The board wires the PAG7982 over an **8-bit parallel** bus (not
 * MIPI-CSI) with inverted H/V sync, and the controller DMAs whole
 * frames straight into the video buffer the driver hands it.  The pixel
 * bus itself is not modelled: there is nothing on the guest side that
 * can observe it, so this model synthesises what the sensor would have
 * driven and writes it to CAM_FRAME_ADDR as a bus master.  The sensor's
 * I2C register file lives in halo_pag7982.c.
 *
 * Register contract — only what the driver touches gets behavior, the
 * rest is plain storage:
 *  - CAM_CTRL (0x00): START (bit 0) begins one frame; BUSY (bit 2) is
 *    read-only and set while a frame is in flight (cam_stream_start
 *    refuses to start on BUSY, cam_stream_stop and cam_flush poll it
 *    down); SW_RESET (bit 8) aborts and clears the interrupt status;
 *    SNAPSHOT (bit 4) and FIFO_CLK_SEL (bit 12) are recorded and
 *    ignored.  The devicetree leaves capture-mode at its "snapshot"
 *    default, and the driver restarts capture per frame from the STOP
 *    interrupt either way (cam_work_helper), so one frame per START is
 *    the only behavior needed.
 *  - CAM_INTR (0x04) is the raw status, write-one-to-clear;
 *    CAM_INTR_ENA (0x08) gates the NVIC line.  A completed frame raises
 *    VSYNC | STOP together: the ISR reads INTR & INTR_ENA, writes it
 *    back and hands the buffer to its work queue on STOP.
 *  - CAM_VIDEO_FCFG (0x28) carries the geometry the driver programmed
 *    from the sensor format: DATA[13:0] = width, ROW[27:16] =
 *    height - 1 (cam_set_fmt).
 *  - CAM_CFG (0x10) DATA_MODE[18:16] gives the bus width; 8-bit (3) is
 *    the board's wiring and the only one with a defined pixel layout
 *    here.
 *  - CAM_FRAME_ADDR (0x30) is a bus-master address.  With
 *    CONFIG_FB_USES_DTCM_REGION=y the driver writes the 0x58800000
 *    global alias of the DTCM buffer; both that and the local
 *    0x20000000 address are mapped in system memory, so a plain
 *    address_space write resolves either.
 *
 * Frame source.  The sensor's output format is 8-bit Bayer BGGR
 * (VIDEO_PIX_FMT_BGGR8, 640x480 — the one entry in the driver's format
 * caps), which the firmware debayers and JPEG-encodes in libmpix.  So
 * the model mosaics an RGB source down to one byte per pixel:
 *   even rows  B G B G ...
 *   odd rows   G R G R ...
 * The RGB source is either a frame file (machine option cam-file, and
 * the runtime "camera-file" property) or, with none given, a built-in
 * deterministic gradient so that frame.camera works out of the box.
 * The frame file is written by halo-emu from a PNG/PNM/JPEG/MJPEG or
 * its own test pattern (tools/camera_source.py), which is where all
 * image decoding lives — this build of QEMU has neither libpng nor
 * libjpeg.  Its layout is:
 *
 *   0x00  "HALOCAM1"
 *   0x08  uint32 width, height, frames, interval_ms   (little-endian)
 *   0x18  frames x width x height x 3 bytes, RGB888, top-down
 *
 * Scaling to the geometry the guest asked for is nearest-neighbour, and
 * multi-frame sources advance exactly one frame per delivered frame —
 * deterministic rather than wall-clock-driven, so a test sees the same
 * frames in the same order every run.  At the 30 fps the sensor is
 * configured for (devicetree frame-rate, and the interval this model
 * paces START with) that is also real time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "qom/object.h"

#define TYPE_HALO_LPCAM "halo-lpcam"
OBJECT_DECLARE_SIMPLE_TYPE(HaloLpcamState, HALO_LPCAM)

#define R_CAM_CTRL         0x00
#define R_CAM_INTR         0x04
#define R_CAM_INTR_ENA     0x08
#define R_CAM_CFG          0x10
#define R_CAM_AXI_ERR_STAT 0x18
#define R_CAM_VIDEO_FCFG   0x28
#define R_CAM_FRAME_ADDR   0x30

#define CTRL_SW_RESET      (1u << 8)
#define CTRL_BUSY          (1u << 2)
#define CTRL_START         (1u << 0)

#define INTR_VSYNC         (1u << 16)
#define INTR_STOP          (1u << 0)

/* CAM_CFG DATA_MODE[18:16]; 3 = CPI_DATA_MODE_8_BIT (the board wiring) */
#define CFG_DATA_MODE_SHIFT 16
#define CFG_DATA_MODE_MASK  0x7
#define CPI_DATA_MODE_8_BIT 3

#define LPCAM_NUM_REGS     (0x1000 / 4)

/* Sensor frame rate: devicetree frame-rate = <30> on pag7982 */
#define LPCAM_FRAME_MS     33

/* Geometry sanity clamps: the register fields are 14- and 12-bit */
#define LPCAM_MAX_WIDTH    0x3FFF
#define LPCAM_MAX_HEIGHT   0x1000

#define LPCAM_MAGIC        "HALOCAM1"
#define LPCAM_MAGIC_LEN    8
#define LPCAM_HDR_LEN      24
#define LPCAM_MAX_FILE     (256 * MiB)

struct HaloLpcamState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer frame_timer;
    qemu_irq irq;

    uint32_t reg[LPCAM_NUM_REGS];
    uint32_t intr;
    uint32_t intr_ena;
    bool busy;

    /* Frame source */
    char *cam_file;          /* creation property, then the live path */
    uint8_t *src;            /* frames x src_w x src_h x 3, RGB888 */
    uint32_t src_w, src_h;
    uint32_t src_frames;
    uint32_t src_interval_ms;
    uint32_t frame_idx;      /* next source frame to deliver */
    uint32_t captures;       /* frames delivered since reset */

    uint8_t *linebuf;
    uint32_t linebuf_len;
    bool warned_data_mode;
    bool warned_no_buffer;
};

static void halo_lpcam_update_irq(HaloLpcamState *s)
{
    qemu_set_irq(s->irq, !!(s->intr & s->intr_ena));
}

static uint32_t halo_lpcam_interval_ms(HaloLpcamState *s)
{
    if (s->src && s->src_interval_ms) {
        return s->src_interval_ms;
    }
    return LPCAM_FRAME_MS;
}

/* ------------------------------------------------------------------ */
/* Frame source                                                        */
/* ------------------------------------------------------------------ */

static void halo_lpcam_drop_source(HaloLpcamState *s)
{
    g_free(s->src);
    s->src = NULL;
    s->src_w = s->src_h = 0;
    s->src_frames = 0;
    s->src_interval_ms = 0;
    s->frame_idx = 0;
}

static bool halo_lpcam_load_source(HaloLpcamState *s, const char *path,
                                   Error **errp)
{
    g_autofree char *blob = NULL;
    uint32_t w, h, frames, interval;
    const uint8_t *hdr;
    gsize len = 0;
    GError *gerr = NULL;
    uint64_t need;

    if (!path || !path[0]) {
        halo_lpcam_drop_source(s);
        g_free(s->cam_file);
        s->cam_file = NULL;
        return true;
    }

    if (!g_file_get_contents(path, &blob, &len, &gerr)) {
        error_setg(errp, "cannot read camera frame file: %s", gerr->message);
        g_error_free(gerr);
        return false;
    }
    if (len < LPCAM_HDR_LEN || len > LPCAM_MAX_FILE) {
        error_setg(errp, "%s is %zu bytes: not a camera frame file",
                   path, (size_t)len);
        return false;
    }
    hdr = (const uint8_t *)blob;
    if (memcmp(hdr, LPCAM_MAGIC, LPCAM_MAGIC_LEN) != 0) {
        error_setg(errp, "%s does not start with " LPCAM_MAGIC
                   " — halo-emu writes these files, see --camera", path);
        return false;
    }
    w = ldl_le_p(hdr + 8);
    h = ldl_le_p(hdr + 12);
    frames = ldl_le_p(hdr + 16);
    interval = ldl_le_p(hdr + 20);
    if (w < 1 || w > LPCAM_MAX_WIDTH || h < 1 || h > LPCAM_MAX_HEIGHT ||
        frames < 1) {
        error_setg(errp, "%s has an implausible geometry %ux%ux%u",
                   path, w, h, frames);
        return false;
    }
    need = (uint64_t)frames * w * h * 3 + LPCAM_HDR_LEN;
    if (need > len) {
        error_setg(errp, "%s is truncated: %ux%ux%u needs %" PRIu64
                   " bytes, file is %zu", path, w, h, frames, need,
                   (size_t)len);
        return false;
    }

    halo_lpcam_drop_source(s);
    s->src = g_memdup2(blob + LPCAM_HDR_LEN, need - LPCAM_HDR_LEN);
    s->src_w = w;
    s->src_h = h;
    s->src_frames = frames;
    s->src_interval_ms = interval;
    g_free(s->cam_file);
    s->cam_file = g_strdup(path);
    return true;
}

/*
 * The RGB triple the sensor would have seen at output pixel (x, y) of a
 * w x h frame.  With no source loaded this is a deterministic gradient
 * (red along x, green along y, an xor texture in blue) that makes the
 * camera usable — and visibly synthetic — with no --camera given.
 */
static void halo_lpcam_source_rgb(HaloLpcamState *s, uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h, uint8_t rgb[3])
{
    const uint8_t *px;
    uint32_t sx, sy;

    if (!s->src) {
        rgb[0] = w > 1 ? x * 255 / (w - 1) : 0;
        rgb[1] = h > 1 ? y * 255 / (h - 1) : 0;
        rgb[2] = (x ^ y) & 0xff;
        return;
    }

    sx = (w == s->src_w) ? x : (uint32_t)((uint64_t)x * s->src_w / w);
    sy = (h == s->src_h) ? y : (uint32_t)((uint64_t)y * s->src_h / h);
    px = s->src + ((size_t)s->frame_idx * s->src_h * s->src_w +
                   (size_t)sy * s->src_w + sx) * 3;
    rgb[0] = px[0];
    rgb[1] = px[1];
    rgb[2] = px[2];
}

/*
 * Mosaic one row into the Bayer BGGR8 byte stream the parallel bus
 * carries: even rows alternate B,G from x=0, odd rows G,R.
 */
static void halo_lpcam_bayer_row(HaloLpcamState *s, uint32_t y, uint32_t w,
                                 uint32_t h, uint32_t bpp)
{
    for (uint32_t x = 0; x < w; x++) {
        uint8_t rgb[3];
        unsigned chan;

        halo_lpcam_source_rgb(s, x, y, w, h, rgb);
        if (y & 1) {
            chan = (x & 1) ? 0 : 1; /* R : G */
        } else {
            chan = (x & 1) ? 1 : 2; /* G : B */
        }
        memset(s->linebuf + (size_t)x * bpp, rgb[chan], bpp);
    }
}

static void halo_lpcam_geometry(HaloLpcamState *s, uint32_t *w, uint32_t *h,
                                uint32_t *bpp)
{
    uint32_t fcfg = s->reg[R_CAM_VIDEO_FCFG / 4];
    uint32_t mode = (s->reg[R_CAM_CFG / 4] >> CFG_DATA_MODE_SHIFT) &
                    CFG_DATA_MODE_MASK;

    *w = fcfg & LPCAM_MAX_WIDTH;
    /* ROW holds height - 1, so an unprogrammed register is 0x0, not
     * a one-line frame. */
    *h = fcfg ? ((fcfg >> 16) & (LPCAM_MAX_HEIGHT - 1)) + 1 : 0;
    *bpp = 1;
    if (mode > CPI_DATA_MODE_8_BIT) {
        /*
         * Wider buses carry a different pixel layout (RAW10..RAW16),
         * which the halo sensor never uses.  Keep the frame the right
         * size and replicate the mosaic byte rather than corrupting it
         * silently.
         */
        *bpp = 2;
        if (!s->warned_data_mode) {
            s->warned_data_mode = true;
            qemu_log_mask(LOG_UNIMP, "halo-lpcam: data-mode %u is not "
                          "modelled; frames stay 8-bit Bayer\n", mode);
        }
    }
}

/* DMA one frame into the buffer at CAM_FRAME_ADDR. */
static void halo_lpcam_deliver(HaloLpcamState *s)
{
    uint32_t w, h, bpp, stride;
    hwaddr addr = s->reg[R_CAM_FRAME_ADDR / 4];

    halo_lpcam_geometry(s, &w, &h, &bpp);
    if (!w || !h) {
        return; /* no format programmed yet: nothing to write */
    }
    if (!addr) {
        if (!s->warned_no_buffer) {
            s->warned_no_buffer = true;
            qemu_log_mask(LOG_GUEST_ERROR, "halo-lpcam: capture started "
                          "with CAM_FRAME_ADDR = 0; frame dropped\n");
        }
        return;
    }

    stride = w * bpp;
    if (s->linebuf_len < stride) {
        g_free(s->linebuf);
        s->linebuf = g_malloc(stride);
        s->linebuf_len = stride;
    }

    for (uint32_t y = 0; y < h; y++) {
        halo_lpcam_bayer_row(s, y, w, h, bpp);
        if (address_space_write(&address_space_memory,
                                addr + (hwaddr)y * stride,
                                MEMTXATTRS_UNSPECIFIED,
                                s->linebuf, stride) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "halo-lpcam: frame DMA to "
                          "0x%" HWADDR_PRIx " failed\n", addr);
            return;
        }
    }

    s->captures++;
    if (s->src_frames) {
        s->frame_idx = (s->frame_idx + 1) % s->src_frames;
    }
}

static void halo_lpcam_frame_tick(void *opaque)
{
    HaloLpcamState *s = opaque;

    if (!s->busy) {
        return;
    }
    halo_lpcam_deliver(s);
    s->busy = false;
    /* One frame per START: the driver re-arms from its STOP handler. */
    s->reg[R_CAM_CTRL / 4] &= ~CTRL_START;
    s->intr |= INTR_VSYNC | INTR_STOP;
    halo_lpcam_update_irq(s);
}

static void halo_lpcam_stop(HaloLpcamState *s)
{
    timer_del(&s->frame_timer);
    s->busy = false;
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

static uint64_t halo_lpcam_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloLpcamState *s = opaque;

    switch (offset) {
    case R_CAM_CTRL:
        return (s->reg[R_CAM_CTRL / 4] & ~CTRL_BUSY) |
               (s->busy ? CTRL_BUSY : 0);
    case R_CAM_INTR:
        return s->intr;
    case R_CAM_INTR_ENA:
        return s->intr_ena;
    case R_CAM_AXI_ERR_STAT:
        return 0; /* no AXI errors are ever injected */
    default:
        return s->reg[offset / 4];
    }
}

static void halo_lpcam_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    HaloLpcamState *s = opaque;

    switch (offset) {
    case R_CAM_CTRL:
        if (value & CTRL_SW_RESET) {
            halo_lpcam_stop(s);
            s->reg[R_CAM_CTRL / 4] = 0;
            s->intr = 0;
            halo_lpcam_update_irq(s);
            return;
        }
        s->reg[R_CAM_CTRL / 4] = value & ~CTRL_BUSY;
        if (value & CTRL_START) {
            if (!s->busy) {
                s->busy = true;
                timer_mod(&s->frame_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                          halo_lpcam_interval_ms(s));
            }
        } else {
            halo_lpcam_stop(s);
        }
        return;
    case R_CAM_INTR:
        s->intr &= ~(uint32_t)value; /* write-one-to-clear */
        halo_lpcam_update_irq(s);
        return;
    case R_CAM_INTR_ENA:
        s->intr_ena = value;
        halo_lpcam_update_irq(s);
        return;
    case R_CAM_AXI_ERR_STAT:
        return;
    default:
        s->reg[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps halo_lpcam_ops = {
    .read = halo_lpcam_read,
    .write = halo_lpcam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* QOM                                                                 */
/* ------------------------------------------------------------------ */

static char *halo_lpcam_get_file(Object *obj, Error **errp)
{
    HaloLpcamState *s = HALO_LPCAM(obj);

    return g_strdup(s->cam_file ? s->cam_file : "");
}

static void halo_lpcam_set_file(Object *obj, const char *value, Error **errp)
{
    halo_lpcam_load_source(HALO_LPCAM(obj), value, errp);
}

static char *halo_lpcam_get_source(Object *obj, Error **errp)
{
    HaloLpcamState *s = HALO_LPCAM(obj);

    return g_strdup(s->src ? "file" : "gradient");
}

static void halo_lpcam_get_dim(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    HaloLpcamState *s = HALO_LPCAM(obj);
    uint32_t w, h, bpp, value;

    halo_lpcam_geometry(s, &w, &h, &bpp);
    value = !strcmp(name, "camera-width") ? w : h;
    visit_type_uint32(v, name, &value, errp);
}

static void halo_lpcam_reset(DeviceState *dev)
{
    HaloLpcamState *s = HALO_LPCAM(dev);

    halo_lpcam_stop(s);
    memset(s->reg, 0, sizeof(s->reg));
    s->intr = 0;
    s->intr_ena = 0;
    s->captures = 0;
    s->frame_idx = 0;
    halo_lpcam_update_irq(s);
    /* The loaded frame source survives reset, like the audio models'
     * WAV files: the control socket may have set it before a reboot. */
}

static void halo_lpcam_realize(DeviceState *dev, Error **errp)
{
    HaloLpcamState *s = HALO_LPCAM(dev);

    if (s->cam_file) {
        g_autofree char *path = g_strdup(s->cam_file);

        g_clear_pointer(&s->cam_file, g_free);
        if (!halo_lpcam_load_source(s, path, errp)) {
            return;
        }
    }
    timer_init_ms(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                  halo_lpcam_frame_tick, s);
}

static void halo_lpcam_unrealize(DeviceState *dev)
{
    HaloLpcamState *s = HALO_LPCAM(dev);

    timer_del(&s->frame_timer);
    halo_lpcam_drop_source(s);
    g_clear_pointer(&s->linebuf, g_free);
    s->linebuf_len = 0;
}

static void halo_lpcam_init(Object *obj)
{
    HaloLpcamState *s = HALO_LPCAM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_lpcam_ops, s,
                          TYPE_HALO_LPCAM, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    object_property_add_str(obj, "camera-file", halo_lpcam_get_file,
                            halo_lpcam_set_file);
    object_property_add_str(obj, "camera-source", halo_lpcam_get_source,
                            NULL);
    object_property_add_uint32_ptr(obj, "camera-frames", &s->src_frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "camera-frame", &s->frame_idx,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "camera-captures", &s->captures,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "camera-width", "uint32", halo_lpcam_get_dim,
                        NULL, NULL, NULL);
    object_property_add(obj, "camera-height", "uint32", halo_lpcam_get_dim,
                        NULL, NULL, NULL);
}

static const Property halo_lpcam_properties[] = {
    DEFINE_PROP_STRING("cam-file", HaloLpcamState, cam_file),
};

static void halo_lpcam_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = halo_lpcam_realize;
    dc->unrealize = halo_lpcam_unrealize;
    dc->desc = "Alif LPCAM parallel camera controller (halo)";
    device_class_set_legacy_reset(dc, halo_lpcam_reset);
    device_class_set_props(dc, halo_lpcam_properties);
}

static const TypeInfo halo_lpcam_types[] = {
    {
        .name = TYPE_HALO_LPCAM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloLpcamState),
        .instance_init = halo_lpcam_init,
        .class_init = halo_lpcam_class_init,
    },
};

DEFINE_TYPES(halo_lpcam_types)
