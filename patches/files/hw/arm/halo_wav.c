/*
 * Halo — minimal 16-bit-PCM RIFF/WAVE reader and writer (see halo_wav.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "halo_wav.h"

#define WAV_HDR_SIZE 44
/* Buffered samples before a write() hits the host file: 4096 mono
 * samples is 128 ms at 32 kHz — small enough that a killed QEMU loses
 * nothing interesting, large enough that the 4 kHz drain tick is not
 * doing a syscall per callback. */
#define WAV_FLUSH_SAMPLES 4096

struct HaloWavWriter {
    FILE *f;
    int rate;
    int channels;
    uint32_t nsamples;           /* total samples handed to the writer */
    int16_t buf[WAV_FLUSH_SAMPLES];
    size_t buf_len;
};

static void wav_put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void wav_put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void wav_write_header(HaloWavWriter *w)
{
    uint32_t data_bytes = w->nsamples * 2;
    uint8_t hdr[WAV_HDR_SIZE];
    long pos = ftell(w->f);

    memcpy(hdr + 0, "RIFF", 4);
    wav_put32(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    wav_put32(hdr + 16, 16);                          /* fmt chunk size */
    wav_put16(hdr + 20, 1);                           /* PCM */
    wav_put16(hdr + 22, w->channels);
    wav_put32(hdr + 24, w->rate);
    wav_put32(hdr + 28, w->rate * w->channels * 2);   /* byte rate */
    wav_put16(hdr + 32, w->channels * 2);             /* block align */
    wav_put16(hdr + 34, 16);                          /* bits/sample */
    memcpy(hdr + 36, "data", 4);
    wav_put32(hdr + 40, data_bytes);

    if (fseek(w->f, 0, SEEK_SET) == 0) {
        (void)fwrite(hdr, 1, sizeof(hdr), w->f);
        if (pos > 0) {
            (void)fseek(w->f, pos, SEEK_SET);
        }
    }
}

HaloWavWriter *halo_wav_writer_open(const char *path, int rate,
                                    int channels, Error **errp)
{
    HaloWavWriter *w;
    FILE *f = fopen(path, "wb");

    if (!f) {
        error_setg_errno(errp, errno, "cannot open '%s' for writing", path);
        return NULL;
    }

    w = g_new0(HaloWavWriter, 1);
    w->f = f;
    w->rate = rate > 0 ? rate : 32000;
    w->channels = channels > 0 ? channels : 1;
    wav_write_header(w);
    /* Header written at offset 0; samples start right after it. */
    (void)fseek(w->f, WAV_HDR_SIZE, SEEK_SET);
    return w;
}

void halo_wav_writer_flush(HaloWavWriter *w)
{
    if (!w) {
        return;
    }
    if (w->buf_len) {
        (void)fwrite(w->buf, sizeof(int16_t), w->buf_len, w->f);
        w->buf_len = 0;
    }
    wav_write_header(w);
    fflush(w->f);
}

void halo_wav_writer_write(HaloWavWriter *w, const int16_t *samples,
                           size_t nsamples)
{
    if (!w) {
        return;
    }
    for (size_t i = 0; i < nsamples; i++) {
        w->buf[w->buf_len++] = samples[i];
        w->nsamples++;
        if (w->buf_len == WAV_FLUSH_SAMPLES) {
            halo_wav_writer_flush(w);
        }
    }
}

void halo_wav_writer_close(HaloWavWriter *w)
{
    if (!w) {
        return;
    }
    halo_wav_writer_flush(w);
    fclose(w->f);
    g_free(w);
}

uint32_t halo_wav_writer_frames(const HaloWavWriter *w)
{
    return w ? w->nsamples / w->channels : 0;
}

int halo_wav_writer_rate(const HaloWavWriter *w)
{
    return w ? w->rate : 0;
}

/* ------------------------------------------------------------------ */

struct HaloWavReader {
    int16_t *samples;   /* mono, downmixed at open */
    size_t nsamples;
    int rate;
    int channels;       /* the file's original channel count */
    size_t pos;         /* playback cursor, in source samples */
    uint32_t frac;      /* nearest-neighbour resampler accumulator */
};

static uint32_t wav_get32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t wav_get16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

HaloWavReader *halo_wav_reader_open(const char *path, Error **errp)
{
    g_autofree char *blob = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    const uint8_t *p;
    size_t off;
    int rate = 0, channels = 0, bits = 0;
    const uint8_t *data = NULL;
    size_t data_bytes = 0;
    HaloWavReader *r;

    if (!g_file_get_contents(path, &blob, &len, &gerr)) {
        error_setg(errp, "cannot read '%s': %s", path, gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    p = (const uint8_t *)blob;
    if (len < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        error_setg(errp, "'%s' is not a RIFF/WAVE file", path);
        return NULL;
    }

    /* Walk the chunk list: only 'fmt ' and 'data' are of interest, and
     * real-world WAVs sprinkle LIST/fact chunks between them. */
    for (off = 12; off + 8 <= len; ) {
        uint32_t csize = wav_get32(p + off + 4);
        const uint8_t *body = p + off + 8;
        size_t avail = len - (off + 8);

        if (csize > avail) {
            csize = avail;
        }
        if (!memcmp(p + off, "fmt ", 4) && csize >= 16) {
            if (wav_get16(body) != 1) {
                error_setg(errp, "'%s': only uncompressed PCM is supported",
                           path);
                return NULL;
            }
            channels = wav_get16(body + 2);
            rate = wav_get32(body + 4);
            bits = wav_get16(body + 14);
        } else if (!memcmp(p + off, "data", 4)) {
            data = body;
            data_bytes = csize;
        }
        off += 8 + csize + (csize & 1); /* chunks are word-aligned */
    }

    if (!rate || channels < 1 || channels > 2) {
        error_setg(errp, "'%s': missing or unsupported fmt chunk", path);
        return NULL;
    }
    if (bits != 16) {
        error_setg(errp, "'%s': only 16-bit samples are supported (got %d)",
                   path, bits);
        return NULL;
    }

    r = g_new0(HaloWavReader, 1);
    r->rate = rate;
    r->channels = channels;
    r->nsamples = data ? data_bytes / (2 * channels) : 0;
    r->samples = g_new0(int16_t, r->nsamples ? r->nsamples : 1);
    for (size_t i = 0; i < r->nsamples; i++) {
        int32_t acc = 0;

        for (int c = 0; c < channels; c++) {
            acc += (int16_t)wav_get16(data + (i * channels + c) * 2);
        }
        r->samples[i] = acc / channels;
    }
    return r;
}

int halo_wav_reader_rate(const HaloWavReader *r)
{
    return r ? r->rate : 0;
}

int halo_wav_reader_channels(const HaloWavReader *r)
{
    return r ? r->channels : 0;
}

void halo_wav_reader_read(HaloWavReader *r, int rate, int16_t *out,
                          size_t nsamples)
{
    if (!r || !r->nsamples || rate <= 0) {
        memset(out, 0, nsamples * sizeof(*out));
        return;
    }

    /*
     * Nearest-neighbour rate conversion: advance the source cursor by
     * src_rate/dst_rate per output sample, carrying the remainder in
     * `frac` so a 16 kHz clip feeding an 8 kHz capture decimates
     * exactly rather than drifting.
     */
    for (size_t i = 0; i < nsamples; i++) {
        out[i] = r->samples[r->pos];
        r->frac += r->rate;
        while (r->frac >= (uint32_t)rate) {
            r->frac -= rate;
            if (++r->pos >= r->nsamples) {
                r->pos = 0; /* loop */
            }
        }
    }
}

void halo_wav_reader_close(HaloWavReader *r)
{
    if (!r) {
        return;
    }
    g_free(r->samples);
    g_free(r);
}
