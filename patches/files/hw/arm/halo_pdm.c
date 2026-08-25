/*
 * Halo — Alif LPPDM @ 0x43002000 (alif,t5838-alif-pdm), IRQ 49: the
 * microphone path.
 *
 * The firmware side is drivers/audio/t5838/t5838_alif_pdm.c.  The board
 * wires `dmas = <&dma2 4 30>` and the driver has no runtime fallback
 * (dma_dev is a compile-time devicetree pointer), so the capture path
 * is always the DMA one: the LPPDM FIFO watermark drives a dma2 (PL330)
 * handshake and the ISR only services the sticky overflow status.  That
 * is why the machine instantiates QEMU's PL330 — see halo.c.
 *
 * Register contract (only what the driver touches gets behavior; the
 * per-channel gain/phase/peak-detect blocks at 0x40..0x7FF are plain
 * storage):
 *  - CONFIG (0x00): bit31 = FIFO clear (write-1, self-clearing);
 *    [23:16] = clock mode, which is also the sample rate
 *    (alif_pdm_get_sample_rate(): 1=8k, 4=16k, 5=32k, 6=48k, 9=96k);
 *    [7:0] = channel-enable mask.  A non-zero mask is what starts and
 *    stops capture (alif_pdm_trigger_start/stop).
 *  - CTL (0x04): bit24 USE_DMA routes the watermark to the DMA
 *    handshake instead of the almost-full interrupt.
 *  - THRESHOLD (0x08): FIFO watermark (the DT sets 4 of the 8 sets).
 *  - FIFO_STATUS (0x0C): sets currently in the FIFO.
 *  - ERROR_IRQ (0x10) bit0: sticky FIFO-overflow status, read to clear.
 *    WARN_IRQ (0x14) bit0: almost-full, a level.  AUDIO_DETECT (0x18):
 *    AAD, always 0 here.  INTERRUPT (0x1C): enable mask, bit0 =
 *    almost-full, bit1 = overflow.
 *  - CH2_CH3_AUDIO_OUT (0x24) is the FIFO read port: reading it drains
 *    one set, and it is the only register the DMA touches.  The other
 *    AUDIO_OUT registers return the set that was last popped without
 *    popping again — the board's mic is channel 2 (mono) or the 2/3
 *    pair (stereo), so nothing reads them in practice.
 *
 *    The read port is width-aware because the DMA does not use 32-bit
 *    beats: Zephyr's dma_pl330.c encodes `source_data_size = 1` (which
 *    the PDM driver's comment calls "native 32-bit") as a **2-byte**
 *    burst beat, and with `source_addr_adj = NO_CHANGE` it issues two
 *    of them at 0x24 to assemble each 32-bit word of its scratch
 *    buffer.  So a 4-byte read pops a set and returns it, while
 *    16-bit reads at 0x24 alternate: the first pops and returns the
 *    low half (ch2), the second returns the high half (ch3) of that
 *    same set.  One FIFO set per 32-bit scratch word either way, which
 *    is what the driver's mono (`word & 0xFFFF`) and stereo (words as
 *    LRLR pairs) extraction both expect.
 *
 * A set is `[ch3:16 | ch2:16]`.  The source is mono, so the sample is
 * placed in both halves: the driver's mono path keeps the low half and
 * its stereo path memcpy's the words as LRLR pairs, and both then see
 * the injected signal.
 *
 * Sample production is a QEMU_CLOCK_VIRTUAL timer at PDM_TICK_HZ,
 * pushing `rate / PDM_TICK_HZ` sets per tick (with the remainder
 * carried) while a channel is enabled.  Source precedence:
 * `wav-in` file > `tone-hz` generator > host capture (`audiodev`) >
 * silence, so an explicitly injected signal always wins over whatever
 * the host microphone happens to hear.
 *
 * DMA handshake: the `dma-req` output line carries QEMU-PL330 *stall*
 * polarity (hw/dma/pl330.c: periph_busy[i] != 0 makes DMAWFP wait), so
 * it is asserted while the FIFO holds less than the watermark and
 * released once a burst's worth of sets is available.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/memory.h"
#include "qom/object.h"
#include "halo_wav.h"

#define TYPE_HALO_PDM "halo-pdm"
OBJECT_DECLARE_SIMPLE_TYPE(HaloPdmState, HALO_PDM)

#define R_CONFIG       0x00
#define R_CTL          0x04
#define R_THRESHOLD    0x08
#define R_FIFO_STATUS  0x0C
#define R_ERROR_IRQ    0x10
#define R_WARN_IRQ     0x14
#define R_AUDIO_DETECT 0x18
#define R_INTERRUPT    0x1C
#define R_CH0_CH1_OUT  0x20
#define R_CH2_CH3_OUT  0x24
#define R_CH4_CH5_OUT  0x28
#define R_CH6_CH7_OUT  0x2C

#define PDM_BLOCK_SIZE 0x1000
#define PDM_NUM_REGS   (PDM_BLOCK_SIZE / 4)

#define CONFIG_FIFO_CLEAR (1u << 31)
#define CONFIG_MODE_SHIFT 16
#define CONFIG_MODE_MASK  0xFFu
#define CONFIG_CHAN_MASK  0xFFu

#define CTL_USE_DMA (1u << 24)

#define IRQ_ALMOST_FULL (1u << 0)
#define IRQ_OVERFLOW    (1u << 1)
#define ERR_OVERFLOW    (1u << 0)

/* 8 sets deep — the FIFO the driver's comments size their headroom
 * against (PDM_FIFO_DEPTH_FRAMES). */
#define PDM_FIFO_DEPTH 8
#define PDM_TICK_HZ    4000

struct HaloPdmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req;   /* PL330 stall line (1 = make DMAWFP wait) */
    QEMUTimer *tick;

    uint32_t regs[PDM_NUM_REGS];

    uint32_t fifo[PDM_FIFO_DEPTH];
    unsigned level;
    uint32_t last_set;  /* what the non-popping AUDIO_OUT reads return */
    bool half_pending;  /* a 16-bit read port beat owes its high half */
    bool overflow;      /* sticky ERROR_IRQ bit0 */
    bool warned_empty;
    bool dma_stalled;

    uint32_t frac;      /* sample-rate remainder carried between ticks */

    /* Sources */
    char *wav_in_path;
    HaloWavReader *wav;
    uint32_t tone_hz;
    uint32_t tone_amplitude;
    uint32_t tone_phase;
    AudioBackend *audio_be;
    SWVoiceIn *voice;
    int voice_rate;

    uint32_t nsamples;
};

/* ------------------------------------------------------------------ */
/* Configuration decoding                                              */
/* ------------------------------------------------------------------ */

static bool halo_pdm_running(HaloPdmState *s)
{
    return (s->regs[R_CONFIG / 4] & CONFIG_CHAN_MASK) != 0;
}

static uint32_t halo_pdm_rate(HaloPdmState *s)
{
    uint32_t mode = (s->regs[R_CONFIG / 4] >> CONFIG_MODE_SHIFT) &
                    CONFIG_MODE_MASK;

    /* alif_pdm_get_sample_rate() */
    switch (mode) {
    case 4: return 16000;  /* HIGH_QUALITY_1024 */
    case 5: return 32000;  /* WIDE_BANDWIDTH_1536 */
    case 6: return 48000;  /* FULL_BANDWIDTH_2400 */
    case 9: return 96000;  /* ULTRASOUND_96 */
    default: return 8000;  /* STANDARD_VOICE_512 and anything unknown */
    }
}

static unsigned halo_pdm_watermark(HaloPdmState *s)
{
    unsigned wm = s->regs[R_THRESHOLD / 4] & 0xF;

    if (wm == 0 || wm > PDM_FIFO_DEPTH) {
        wm = PDM_FIFO_DEPTH / 2;
    }
    return wm;
}

/* ------------------------------------------------------------------ */
/* Sources                                                             */
/* ------------------------------------------------------------------ */

static void halo_pdm_audio_cb(void *opaque, int avail)
{
    /* The tick pulls; nothing to do on demand. */
}

static const char *halo_pdm_source(HaloPdmState *s)
{
    if (s->wav) {
        return "wav";
    }
    if (s->tone_hz) {
        return "tone";
    }
    if (s->audio_be) {
        return "host";
    }
    return "silence";
}

static void halo_pdm_fill(HaloPdmState *s, int16_t *out, unsigned n,
                          uint32_t rate)
{
    if (s->wav) {
        halo_wav_reader_read(s->wav, rate, out, n);
        return;
    }
    if (s->tone_hz) {
        /*
         * Integer sine from the phase accumulator: 2^32 of phase per
         * cycle, indexed into a quarter-wave-free plain sinf() would
         * pull in libm for no benefit, so use the small-angle-free
         * approach of QEMU's own test tones — a 64-entry table is
         * plenty for a test signal.
         */
        static const int16_t sine64[64] = {
                 0,   3211,   6392,   9511,  12539,  15446,  18204,  20787,
             23169,  25329,  27244,  28897,  30272,  31356,  32137,  32609,
             32767,  32609,  32137,  31356,  30272,  28897,  27244,  25329,
             23169,  20787,  18204,  15446,  12539,   9511,   6392,   3211,
                 0,  -3211,  -6392,  -9511, -12539, -15446, -18204, -20787,
            -23169, -25329, -27244, -28897, -30272, -31356, -32137, -32609,
            -32767, -32609, -32137, -31356, -30272, -28897, -27244, -25329,
            -23169, -20787, -18204, -15446, -12539,  -9511,  -6392,  -3211,
        };
        uint32_t step = (uint32_t)(((uint64_t)s->tone_hz << 32) / rate);
        int32_t amp = MIN(s->tone_amplitude, 32767);

        for (unsigned i = 0; i < n; i++) {
            out[i] = (int16_t)((sine64[s->tone_phase >> 26] * amp) / 32767);
            s->tone_phase += step;
        }
        return;
    }
    if (s->audio_be) {
        size_t want = n * sizeof(*out);
        size_t got;

        if (s->voice && s->voice_rate != (int)rate) {
            audio_be_close_in(s->audio_be, s->voice);
            s->voice = NULL;
        }
        if (!s->voice) {
            struct audsettings as = {
                .freq = rate,
                .nchannels = 1,
                .fmt = AUDIO_FORMAT_S16,
                .big_endian = 0,
            };

            s->voice = audio_be_open_in(s->audio_be, NULL, "halo-mic", s,
                                        halo_pdm_audio_cb, &as);
            if (!s->voice) {
                warn_report("halo-pdm: cannot open the audio backend for "
                            "capture; the microphone reads silence");
                s->audio_be = NULL;
            } else {
                s->voice_rate = rate;
                audio_be_set_active_in(s->audio_be, s->voice, true);
            }
        }
        if (s->voice) {
            got = audio_be_read(s->audio_be, s->voice, out, want);
            if (got < want) {
                /* Host capture starved: pad rather than stall the
                 * guest's capture timeline. */
                memset((uint8_t *)out + got, 0, want - got);
            }
            return;
        }
    }
    memset(out, 0, n * sizeof(*out));
}

/* ------------------------------------------------------------------ */
/* FIFO / IRQ / DMA handshake                                          */
/* ------------------------------------------------------------------ */

static void halo_pdm_update(HaloPdmState *s)
{
    uint32_t enable = s->regs[R_INTERRUPT / 4];
    unsigned wm = halo_pdm_watermark(s);
    bool warn = halo_pdm_running(s) && s->level >= wm;
    bool level = ((enable & IRQ_ALMOST_FULL) && warn) ||
                 ((enable & IRQ_OVERFLOW) && s->overflow);
    bool stall;

    qemu_set_irq(s->irq, level);

    /* Hold the PL330 off until a burst's worth of sets is queued.  The
     * line is only meaningful with USE_DMA; without it, keep the DMA
     * parked so a stale channel program cannot steal sets from the
     * almost-full ISR path. */
    stall = !((s->regs[R_CTL / 4] & CTL_USE_DMA) && s->level >= wm);
    if (stall != s->dma_stalled) {
        s->dma_stalled = stall;
        qemu_set_irq(s->dma_req, stall);
    }
}

static void halo_pdm_push(HaloPdmState *s, int16_t sample)
{
    uint32_t set = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;

    if (s->level >= PDM_FIFO_DEPTH) {
        s->overflow = true;
        return;
    }
    s->fifo[s->level++] = set;
    s->nsamples++;
}

static uint32_t halo_pdm_pop(HaloPdmState *s)
{
    if (!s->level) {
        /* An over-read: the driver's own notes call this out as the
         * source of audible static on hardware, so make it visible
         * instead of returning a random value.  Logged once — a
         * mis-tuned handshake would otherwise flood. */
        if (!s->warned_empty) {
            s->warned_empty = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "halo-pdm: FIFO read while empty (further "
                          "occurrences not logged)\n");
        }
        return s->last_set;
    }
    s->last_set = s->fifo[0];
    memmove(s->fifo, s->fifo + 1, (--s->level) * sizeof(s->fifo[0]));
    return s->last_set;
}

static void halo_pdm_tick(void *opaque)
{
    HaloPdmState *s = opaque;
    uint32_t rate = halo_pdm_rate(s);
    int16_t samples[PDM_FIFO_DEPTH * 4];
    unsigned n;

    s->frac += rate;
    n = s->frac / PDM_TICK_HZ;
    s->frac -= n * PDM_TICK_HZ;
    n = MIN(n, ARRAY_SIZE(samples));

    halo_pdm_fill(s, samples, n, rate);
    for (unsigned i = 0; i < n; i++) {
        halo_pdm_push(s, samples[i]);
    }

    timer_mod_ns(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 NANOSECONDS_PER_SECOND / PDM_TICK_HZ);
    halo_pdm_update(s);
}

static void halo_pdm_set_running(HaloPdmState *s, bool run)
{
    if (!s->tick) {
        return; /* pre-realize */
    }
    if (run) {
        if (!timer_pending(s->tick)) {
            s->frac = 0;
            timer_mod_ns(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         NANOSECONDS_PER_SECOND / PDM_TICK_HZ);
        }
    } else {
        timer_del(s->tick);
    }
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

/* The FIFO read port: see the header comment on the 16-bit DMA beats. */
static uint32_t halo_pdm_read_port(HaloPdmState *s, hwaddr offset,
                                   unsigned size)
{
    if (size >= 4) {
        s->half_pending = false;
        return halo_pdm_pop(s);
    }
    if (offset & 2) {
        /* An explicit high-half address: no pop, just the other half of
         * the set the low-half read already took. */
        return s->last_set >> 16;
    }
    if (s->half_pending) {
        s->half_pending = false;
        return s->last_set >> 16;
    }
    s->half_pending = true;
    return halo_pdm_pop(s) & 0xFFFF;
}

static uint64_t halo_pdm_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloPdmState *s = opaque;
    hwaddr reg = offset & ~(hwaddr)3;
    unsigned shift = (offset & 3) * 8;
    uint32_t ret;

    switch (reg) {
    case R_FIFO_STATUS:
        ret = s->level;
        break;
    case R_ERROR_IRQ:
        ret = s->overflow ? ERR_OVERFLOW : 0;
        s->overflow = false;
        halo_pdm_update(s);
        break;
    case R_WARN_IRQ:
        ret = (halo_pdm_running(s) && s->level >= halo_pdm_watermark(s))
              ? IRQ_ALMOST_FULL : 0;
        break;
    case R_AUDIO_DETECT:
        ret = 0; /* AAD is not modelled; reading also acks it */
        break;
    case R_CH2_CH3_OUT:
        ret = halo_pdm_read_port(s, offset, size);
        halo_pdm_update(s);
        return ret; /* already the right width */
    case R_CH0_CH1_OUT:
    case R_CH4_CH5_OUT:
    case R_CH6_CH7_OUT:
        ret = s->last_set;
        break;
    default:
        ret = s->regs[reg / 4];
        break;
    }
    return ret >> shift;
}

static void halo_pdm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloPdmState *s = opaque;
    hwaddr reg = offset & ~(hwaddr)3;
    unsigned shift = (offset & 3) * 8;
    uint32_t merged = s->regs[reg / 4];

    if (size >= 4) {
        merged = value;
    } else {
        uint32_t mask = ((1u << (size * 8)) - 1) << shift;

        merged = (merged & ~mask) | ((uint32_t)value << shift);
    }

    switch (reg) {
    case R_CONFIG:
        if (merged & CONFIG_FIFO_CLEAR) {
            /* write-1 clear is self-clearing and carries no config */
            s->level = 0;
            s->half_pending = false;
            halo_pdm_update(s);
            return;
        }
        s->regs[R_CONFIG / 4] = merged;
        halo_pdm_set_running(s, halo_pdm_running(s));
        halo_pdm_update(s);
        return;
    case R_FIFO_STATUS:
    case R_ERROR_IRQ:
    case R_WARN_IRQ:
    case R_AUDIO_DETECT:
    case R_CH0_CH1_OUT:
    case R_CH2_CH3_OUT:
    case R_CH4_CH5_OUT:
    case R_CH6_CH7_OUT:
        return; /* read-only */
    default:
        s->regs[reg / 4] = merged;
        halo_pdm_update(s);
        return;
    }
}

static const MemoryRegionOps halo_pdm_ops = {
    .read = halo_pdm_read,
    .write = halo_pdm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* The PL330 reads the FIFO port in 16-bit beats (see above), so
     * sub-word accesses reach the handler instead of being widened. */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* QOM                                                                 */
/* ------------------------------------------------------------------ */

static void halo_pdm_open_wav(HaloPdmState *s, const char *path,
                              Error **errp)
{
    HaloWavReader *r = NULL;

    if (path && path[0]) {
        r = halo_wav_reader_open(path, errp);
        if (!r) {
            return;
        }
    }
    halo_wav_reader_close(s->wav);
    s->wav = r;
    g_free(s->wav_in_path);
    s->wav_in_path = (path && path[0]) ? g_strdup(path) : NULL;
}

static char *halo_pdm_get_wav_in(Object *obj, Error **errp)
{
    HaloPdmState *s = HALO_PDM(obj);

    return g_strdup(s->wav_in_path ? s->wav_in_path : "");
}

static void halo_pdm_set_wav_in(Object *obj, const char *value, Error **errp)
{
    halo_pdm_open_wav(HALO_PDM(obj), value, errp);
}

static char *halo_pdm_get_source(Object *obj, Error **errp)
{
    return g_strdup(halo_pdm_source(HALO_PDM(obj)));
}

static void halo_pdm_reset(DeviceState *dev)
{
    HaloPdmState *s = HALO_PDM(dev);

    if (s->tick) {
        timer_del(s->tick);
    }
    memset(s->regs, 0, sizeof(s->regs));
    s->level = 0;
    s->last_set = 0;
    s->half_pending = false;
    s->overflow = false;
    s->frac = 0;
    s->tone_phase = 0;
    qemu_set_irq(s->irq, 0);
    /* Park the DMA: nothing to hand it until capture restarts. */
    s->dma_stalled = true;
    qemu_set_irq(s->dma_req, 1);
}

static void halo_pdm_realize(DeviceState *dev, Error **errp)
{
    HaloPdmState *s = HALO_PDM(dev);

    if (s->audio_be && !audio_be_check(&s->audio_be, errp)) {
        return;
    }
    if (s->wav_in_path) {
        g_autofree char *path = g_strdup(s->wav_in_path);
        Error *err = NULL;

        g_clear_pointer(&s->wav_in_path, g_free);
        halo_pdm_open_wav(s, path, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, halo_pdm_tick, s);
}

static void halo_pdm_unrealize(DeviceState *dev)
{
    HaloPdmState *s = HALO_PDM(dev);

    if (s->voice) {
        audio_be_close_in(s->audio_be, s->voice);
        s->voice = NULL;
    }
    halo_wav_reader_close(s->wav);
    s->wav = NULL;
}

static void halo_pdm_init(Object *obj)
{
    HaloPdmState *s = HALO_PDM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_pdm_ops, s, "halo-pdm",
                          PDM_BLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->dma_req, "dma-req", 1);

    object_property_add_str(obj, "mic-wav-in", halo_pdm_get_wav_in,
                            halo_pdm_set_wav_in);
    object_property_add_uint32_ptr(obj, "mic-tone-hz", &s->tone_hz,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mic-tone-amplitude",
                                   &s->tone_amplitude,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mic-samples", &s->nsamples,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "mic-source", halo_pdm_get_source, NULL);
}

static const Property halo_pdm_properties[] = {
    DEFINE_PROP_STRING("wav-in", HaloPdmState, wav_in_path),
    DEFINE_PROP_UINT32("tone-hz", HaloPdmState, tone_hz, 0),
    DEFINE_PROP_UINT32("tone-amplitude", HaloPdmState, tone_amplitude, 8000),
    DEFINE_AUDIO_PROPERTIES(HaloPdmState, audio_be),
};

static void halo_pdm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = halo_pdm_realize;
    dc->unrealize = halo_pdm_unrealize;
    device_class_set_legacy_reset(dc, halo_pdm_reset);
    device_class_set_props(dc, halo_pdm_properties);
}

static const TypeInfo halo_pdm_types[] = {
    {
        .name = TYPE_HALO_PDM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloPdmState),
        .instance_init = halo_pdm_init,
        .class_init = halo_pdm_class_init,
    },
};

DEFINE_TYPES(halo_pdm_types)
