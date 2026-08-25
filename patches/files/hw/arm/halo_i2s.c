/*
 * Halo — Alif I2S0 @ 0x49014000 (alif,i2s-sync), IRQ 141: the speaker
 * path (MAX98357A behind it, SD_MODE on gpio8.5).
 *
 * The firmware side is drivers/i2s/i2s_sync/i2s_sync.c.  On this board
 * i2s0 has **no `dmas` property**, so the TX path is the interrupt/FIFO
 * one, not DMA: `i2s_sync_tx_isr_handler()` pushes exactly
 * I2S_FIFO_TRG_LEVEL (8) samples into LTHR/RTHR per ISR entry and
 * returns.  Everything here follows from that:
 *
 *  - 16-entry TX FIFO, trigger level from TFCR (the driver programs 8).
 *    LTHR (0x20) pushes the left sample; RTHR (0x24) pushes the right
 *    one.  In mono the driver duplicates left into right, so only the
 *    left writes are kept — the FIFO holds the sample stream the
 *    firmware actually produced.
 *  - ISR.TXFE (bit 4) = "TX FIFO at or below the trigger level"; the
 *    NVIC line is the plain level `ISR & ~IMR`.  That level (rather
 *    than a pulse) is what makes the guest ISR re-enter until the FIFO
 *    is above the trigger again, exactly like the hardware: empty FIFO
 *    -> 8 samples -> still at the trigger -> 8 more -> full, line
 *    drops.
 *  - ISR.TXFO / TOR is the sticky TX-overrun status, cleared by
 *    reading TOR.  It cannot fire while the driver respects the
 *    trigger, but the driver enables it, so it is modelled.
 *
 * Sample clock: the driver programs the divisor into
 * CLKCTL_PER_SLV->I2S0_CTRL (0x4902F010, plain RAM in the machine),
 * `div = 76.8 MHz / (2 * channels * depth * rate)`.  The rate at which
 * this model must consume *samples* is `rate * channels`, and the
 * channel count cancels: samples/s = 76.8 MHz / (div * 2 * depth),
 * with depth read back from TCR.WLEN.  So a guest-side
 * `frame.speaker.start{sample_rate=8000}` reconfigures the model for
 * free, with no channel-count guessing.
 *
 * Drain: a QEMU_CLOCK_VIRTUAL timer with period `trigger / samples-per-
 * second` (250 us at the DT default of 32 kHz mono) pops up to
 * `trigger` samples per tick into the sink and re-evaluates the IRQ
 * level.  It runs only while TX is enabled and there is either data in
 * the FIFO or an unmasked TXFE — an idle speaker costs nothing.
 *
 * Sink: a WAV file (`wav-out=`) and/or the QEMU audio backend
 * (`audiodev=`), both optional and independent.  With neither, samples
 * are counted and dropped, which is the pre-0032 behavior except that
 * the interrupts now actually fire, so `max98357a_audio_trigger_impl()`
 * no longer burns its 5 s drain-semaphore timeout on every boot.
 *
 * A WAV header carries one sample rate but the guest changes its rate
 * between clips (the boot cue is 16 kHz, `frame.speaker.start` can ask
 * for 8 kHz), so the capture is written at a fixed HALO_WAV_RATE and
 * everything is upsampled into it.  The ratios involved are integers,
 * so this is plain sample repetition — enough for a capture that exists
 * to be inspected, and it keeps a whole session in one correctly-timed
 * file.
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
#include "system/address-spaces.h"
#include "qom/object.h"
#include "halo_wav.h"

#define TYPE_HALO_I2S "halo-i2s"
OBJECT_DECLARE_SIMPLE_TYPE(HaloI2SState, HALO_I2S)

/* Register file (drivers/i2s/i2s_sync/i2s_sync_int.h struct i2s_t) */
#define R_IER    0x000 /* global enable */
#define R_IRER   0x004 /* RX block enable */
#define R_ITER   0x008 /* TX block enable */
#define R_CER    0x00C /* clock enable */
#define R_CCR    0x010 /* clock configuration */
#define R_RXFFR  0x014 /* RX block FIFO reset (WO) */
#define R_TXFFR  0x018 /* TX block FIFO reset (WO) */
#define R_LRBR   0x020 /* left RX buffer / left TX holding */
#define R_RRBR   0x024 /* right RX buffer / right TX holding */
#define R_RER    0x028 /* RX channel enable */
#define R_TER    0x02C /* TX channel enable */
#define R_RCR    0x030 /* RX word length */
#define R_TCR    0x034 /* TX word length */
#define R_ISR    0x038 /* interrupt status (RO) */
#define R_IMR    0x03C /* interrupt mask */
#define R_ROR    0x040 /* RX overrun, read to clear */
#define R_TOR    0x044 /* TX overrun, read to clear */
#define R_RFCR   0x048 /* RX FIFO trigger level */
#define R_TFCR   0x04C /* TX FIFO trigger level */
#define R_RFF    0x050 /* RX channel FIFO reset (WO) */
#define R_TFF    0x054 /* TX channel FIFO reset (WO) */
#define R_RXDMA  0x1C0
#define R_TXDMA  0x1C8
#define R_DMACR  0x200

#define I2S_BLOCK_SIZE 0x1000
#define I2S_NUM_REGS   (I2S_BLOCK_SIZE / 4)

#define ISR_RXDA (1u << 0)
#define ISR_RXFO (1u << 1)
#define ISR_TXFE (1u << 4)
#define ISR_TXFO (1u << 5)

#define ENABLE_BIT (1u << 0)

#define I2S_FIFO_DEPTH 16

/* CLKCTL_PER_SLV->I2S0_CTRL: the divisor the driver programs. */
#define I2S0_CTRL_ADDR    0x4902F010
#define I2S0_CTRL_CKDIV   0x3FF
#define I2S_CLK_SRC_HZ    76800000
#define I2S_CKDIV_DEFAULT 75 /* DT default: 32 kHz mono, 16-bit */

/* TCR.WLEN encoding (enum i2s_wlen_t) */
#define WLEN_MASK 0x7

/* Fixed rate of the wav-out capture; the guest's 8/16/32 kHz clips are
 * upsampled into it (see the header comment). */
#define HALO_WAV_RATE 32000

struct HaloI2SState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *drain;

    uint32_t regs[I2S_NUM_REGS];

    int16_t tx_fifo[I2S_FIFO_DEPTH];
    unsigned tx_level;
    /*
     * Set by the first drain tick after TX is enabled.  Until then
     * TXFE reads 0 even though the FIFO is empty, because the hardware
     * cannot report "below the trigger level" before the block has
     * clocked a sample out.  Reporting it instantly deadlocks the
     * guest: i2s_sync.c's i2s_send() sets dev_data->tx.running *after*
     * i2s_transmitter_start() unmasks the interrupt, and the ISR
     * early-returns while tx.running is false — a level IRQ raised in
     * that window re-enters forever and boot never advances.
     */
    bool tx_started;
    bool tx_overrun;   /* sticky TOR / ISR.TXFO */
    bool rx_overrun;

    /* Sink */
    char *wav_out_path;
    HaloWavWriter *wav;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    int voice_rate;

    uint32_t rate;      /* samples/s, recomputed from CKDIV + TCR */
    uint32_t nsamples;  /* total samples handed to the sink */
    uint32_t wav_frac;  /* wav-out resampler accumulator */
};

/* ------------------------------------------------------------------ */
/* Clocking                                                            */
/* ------------------------------------------------------------------ */

static uint32_t halo_i2s_depth(HaloI2SState *s)
{
    /* enum i2s_wlen_t: 1=12, 2=16, 3=20, 4=24, 5=32 bits */
    static const uint8_t wlen_bits[8] = { 16, 12, 16, 20, 24, 32, 16, 16 };

    return wlen_bits[s->regs[R_TCR / 4] & WLEN_MASK];
}

static uint32_t halo_i2s_rate(HaloI2SState *s)
{
    uint32_t ctrl = address_space_ldl_le(&address_space_memory,
                                         I2S0_CTRL_ADDR,
                                         MEMTXATTRS_UNSPECIFIED, NULL);
    uint32_t div = ctrl & I2S0_CTRL_CKDIV;
    uint32_t depth = halo_i2s_depth(s);

    if (div < 2) {
        div = I2S_CKDIV_DEFAULT;
    }
    /* bclk = 76.8 MHz / div; samples/s = bclk / (2 * depth) */
    return I2S_CLK_SRC_HZ / (div * 2 * depth);
}

static unsigned halo_i2s_trigger(HaloI2SState *s)
{
    unsigned trg = s->regs[R_TFCR / 4] & 0xF;

    return trg ? trg : 1;
}

/* ------------------------------------------------------------------ */
/* Sink                                                                */
/* ------------------------------------------------------------------ */

static void halo_i2s_audio_cb(void *opaque, int avail)
{
    /* The drain tick is the producer; nothing to do on demand. */
}

static void halo_i2s_sink(HaloI2SState *s, int16_t *samples, unsigned n)
{
    if (!n) {
        return;
    }
    s->nsamples += n;

    if (s->wav_out_path && !s->wav) {
        Error *err = NULL;

        s->wav = halo_wav_writer_open(s->wav_out_path, HALO_WAV_RATE, 1,
                                      &err);
        if (!s->wav) {
            warn_report_err(err);
            g_free(s->wav_out_path);
            s->wav_out_path = NULL;
        }
    }
    if (s->wav) {
        /* Upsample into the file's fixed rate by repeating samples, with
         * the remainder carried across calls so nothing drifts. */
        int16_t up[I2S_FIFO_DEPTH * (HALO_WAV_RATE / 8000) + 1];
        unsigned out = 0;

        for (unsigned i = 0; i < n; i++) {
            s->wav_frac += HALO_WAV_RATE;
            while (s->wav_frac >= s->rate && out < ARRAY_SIZE(up)) {
                s->wav_frac -= s->rate;
                up[out++] = samples[i];
            }
        }
        halo_wav_writer_write(s->wav, up, out);
    }

    if (s->audio_be) {
        if (s->voice && s->voice_rate != (int)s->rate) {
            audio_be_close_out(s->audio_be, s->voice);
            s->voice = NULL;
        }
        if (!s->voice) {
            struct audsettings as = {
                .freq = s->rate,
                .nchannels = 1,
                .fmt = AUDIO_FORMAT_S16,
                .big_endian = 0,
            };

            s->voice = audio_be_open_out(s->audio_be, NULL, "halo-speaker",
                                         s, halo_i2s_audio_cb, &as);
            if (!s->voice) {
                warn_report("halo-i2s: cannot open the audio backend; "
                            "playback is silent");
                s->audio_be = NULL;
            } else {
                s->voice_rate = s->rate;
                audio_be_set_active_out(s->audio_be, s->voice, true);
            }
        }
        if (s->voice) {
            /* A short write means the backend is behind; dropping is
             * the right call for a live speaker. */
            audio_be_write(s->audio_be, s->voice, samples,
                           n * sizeof(*samples));
        }
    }
}

/* ------------------------------------------------------------------ */
/* TX FIFO + IRQ + drain                                               */
/* ------------------------------------------------------------------ */

static bool halo_i2s_tx_enabled(HaloI2SState *s)
{
    return (s->regs[R_IER / 4] & ENABLE_BIT) &&
           (s->regs[R_ITER / 4] & ENABLE_BIT) &&
           (s->regs[R_TER / 4] & ENABLE_BIT);
}

static uint32_t halo_i2s_status(HaloI2SState *s)
{
    uint32_t isr = 0;

    if (halo_i2s_tx_enabled(s) && s->tx_started &&
        s->tx_level <= halo_i2s_trigger(s)) {
        isr |= ISR_TXFE;
    }
    if (s->tx_overrun) {
        isr |= ISR_TXFO;
    }
    if (s->rx_overrun) {
        isr |= ISR_RXFO;
    }
    return isr;
}

static void halo_i2s_update(HaloI2SState *s)
{
    uint32_t isr = halo_i2s_status(s);
    bool active;

    qemu_set_irq(s->irq, (isr & ~s->regs[R_IMR / 4]) != 0);

    if (!s->drain) {
        return; /* pre-realize register pokes */
    }

    /* Keep the drain running while there is data to push out, or while
     * the guest is waiting to be asked for more. */
    active = halo_i2s_tx_enabled(s) &&
             (s->tx_level > 0 || !(s->regs[R_IMR / 4] & ISR_TXFE));
    if (active) {
        if (!timer_pending(s->drain)) {
            unsigned trg = halo_i2s_trigger(s);

            s->wav_frac = 0;
    s->rate = halo_i2s_rate(s);
            timer_mod_ns(s->drain, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         (int64_t)trg * NANOSECONDS_PER_SECOND / s->rate);
        }
    } else {
        timer_del(s->drain);
        s->tx_started = false;
        /* End of playback: make the capture complete on disk right
         * away rather than at the next 4096-sample boundary. */
        halo_wav_writer_flush(s->wav);
    }
}

static void halo_i2s_drain(void *opaque)
{
    HaloI2SState *s = opaque;
    int16_t batch[I2S_FIFO_DEPTH];
    unsigned trg = halo_i2s_trigger(s);
    unsigned n = MIN(s->tx_level, trg);

    s->rate = halo_i2s_rate(s);
    s->tx_started = true;

    if (n) {
        memcpy(batch, s->tx_fifo, n * sizeof(*batch));
        memmove(s->tx_fifo, s->tx_fifo + n, (s->tx_level - n) * sizeof(*batch));
        s->tx_level -= n;
        halo_i2s_sink(s, batch, n);
    }

    /* Re-arm before updating so the period always tracks the current
     * rate; halo_i2s_update() cancels it again if TX went idle. */
    timer_mod_ns(s->drain, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 (int64_t)trg * NANOSECONDS_PER_SECOND / s->rate);
    halo_i2s_update(s);
}

static void halo_i2s_tx_push(HaloI2SState *s, uint32_t value)
{
    if (s->tx_level >= I2S_FIFO_DEPTH) {
        s->tx_overrun = true;
        return;
    }
    s->tx_fifo[s->tx_level++] = (int16_t)(value & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

static uint64_t halo_i2s_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloI2SState *s = opaque;
    uint64_t ret;

    switch (offset) {
    case R_ISR:
        return halo_i2s_status(s);
    case R_TOR:
        ret = s->tx_overrun ? 1 : 0;
        s->tx_overrun = false;
        halo_i2s_update(s);
        return ret;
    case R_ROR:
        ret = s->rx_overrun ? 1 : 0;
        s->rx_overrun = false;
        halo_i2s_update(s);
        return ret;
    case R_LRBR:
    case R_RRBR:
    case R_RXDMA:
        /* RX is unused on this board (the mic is the LPPDM, see
         * halo_pdm.c); the FIFO is always empty. */
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void halo_i2s_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloI2SState *s = opaque;

    switch (offset) {
    case R_LRBR: /* LTHR: the left/mono sample */
        halo_i2s_tx_push(s, value);
        halo_i2s_update(s);
        return;
    case R_RRBR: /* RTHR: in mono this duplicates the left sample */
        return;
    case R_TXDMA:
        /* i2s0 has no DMA on this board; a write here would mean the
         * DT grew a txdma and this model needs the DMA path too. */
        qemu_log_mask(LOG_UNIMP, "halo-i2s: TXDMA write (no DMA modelled)\n");
        return;
    case R_TXFFR:
    case R_TFF:
        s->tx_level = 0;
        halo_i2s_update(s);
        return;
    case R_RXFFR:
    case R_RFF:
        return;
    case R_ISR:
    case R_TOR:
    case R_ROR:
        return; /* read-only */
    default:
        s->regs[offset / 4] = value;
        halo_i2s_update(s);
        return;
    }
}

static const MemoryRegionOps halo_i2s_ops = {
    .read = halo_i2s_read,
    .write = halo_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* QOM                                                                 */
/* ------------------------------------------------------------------ */

static bool halo_i2s_get_playing(Object *obj, Error **errp)
{
    HaloI2SState *s = HALO_I2S(obj);

    return halo_i2s_tx_enabled(s) && s->drain && timer_pending(s->drain);
}

static void halo_i2s_reset(DeviceState *dev)
{
    HaloI2SState *s = HALO_I2S(dev);

    if (s->drain) {
        timer_del(s->drain);
    }
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_IMR / 4] = 0xFFFFFFFF; /* all interrupts masked out of reset */
    s->tx_level = 0;
    s->tx_started = false;
    s->tx_overrun = false;
    s->rx_overrun = false;
    s->rate = I2S_CLK_SRC_HZ / (I2S_CKDIV_DEFAULT * 2 * 16);
    qemu_set_irq(s->irq, 0);
    /* The capture file spans the whole session, reboots included — a
     * warm reset must not truncate what has already been recorded. */
    halo_wav_writer_flush(s->wav);
}

static void halo_i2s_realize(DeviceState *dev, Error **errp)
{
    HaloI2SState *s = HALO_I2S(dev);

    if (s->audio_be && !audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->drain = timer_new_ns(QEMU_CLOCK_VIRTUAL, halo_i2s_drain, s);
}

static void halo_i2s_unrealize(DeviceState *dev)
{
    HaloI2SState *s = HALO_I2S(dev);

    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
        s->voice = NULL;
    }
    halo_wav_writer_close(s->wav);
    s->wav = NULL;
}

static void halo_i2s_init(Object *obj)
{
    HaloI2SState *s = HALO_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &halo_i2s_ops, s, "halo-i2s",
                          I2S_BLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    object_property_add_bool(obj, "speaker-playing", halo_i2s_get_playing,
                             NULL);
    object_property_add_uint32_ptr(obj, "speaker-rate", &s->rate,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "speaker-samples", &s->nsamples,
                                   OBJ_PROP_FLAG_READ);
}

static const Property halo_i2s_properties[] = {
    DEFINE_PROP_STRING("wav-out", HaloI2SState, wav_out_path),
    DEFINE_AUDIO_PROPERTIES(HaloI2SState, audio_be),
};

static void halo_i2s_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = halo_i2s_realize;
    dc->unrealize = halo_i2s_unrealize;
    device_class_set_legacy_reset(dc, halo_i2s_reset);
    device_class_set_props(dc, halo_i2s_properties);
}

static const TypeInfo halo_i2s_types[] = {
    {
        .name = TYPE_HALO_I2S,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(HaloI2SState),
        .instance_init = halo_i2s_init,
        .class_init = halo_i2s_class_init,
    },
};

DEFINE_TYPES(halo_i2s_types)
