/*
 * stub_lc3.c — the LC3 codec entry points of the synthetic ROM, backed by
 * Google's liblc3 (ticket 0032).
 *
 * The ten pinned `lc3_api_*` symbols are Alif's thin wrapper around the
 * codec that lives in the Balletto's on-chip ROM.  The firmware uses
 * them for `frame.speaker.start{encoder='lc3'}` /
 * `frame.microphone.start{encoder='lc3'}` and for BLE LE-audio, and it
 * exchanges *real* LC3 bitstreams with the phone (the device test
 * applications/halo/tests/test_speaker_lc3.py plays a stock
 * `female_w1_8k_s16.lc3` file), so a passthrough would not do: this has
 * to be an interoperable codec.  liblc3 is the Bluetooth-SIG-conformant
 * reference-grade implementation, so the job here is purely ABI glue.
 *
 * Memory, and why there is no allocator or slot pool
 * --------------------------------------------------
 * Alif's ABI hands us three opaque, caller-owned structs whose sizes are
 * fixed by *their* implementation (lc3_api.h): cfg 2012 B, encoder
 * 1576 B, decoder 132 B.  liblc3's contexts do not fit those (an encoder
 * is up to ~5.3 KB, a decoder up to ~9 KB), but the ABI also asks *us*
 * how big two other caller-allocated buffers must be:
 *
 *   lc3_api_encoder_scratch_size(cfg)   -> holds the liblc3 encoder
 *   lc3_api_decoder_status_size(cfg)    -> holds the liblc3 decoder
 *
 * So the liblc3 contexts live there, and the opaque structs only carry a
 * small header (magic + configuration + a pointer).  Nothing is
 * allocated, nothing is pooled, and nothing has to survive
 * ble_stack_init() zeroing the ROM data region — every byte of state
 * belongs to a buffer the firmware allocated and owns.
 *
 * The encoder is set up lazily on the first lc3_api_encode_frame(),
 * because that is the first call that sees scratch_mem.  Alif documents
 * scratch as shareable between encoders that never run concurrently; if
 * that ever happens the ownership stamp at the head of the buffer
 * catches it and the encoder is re-initialised (one frame of lost
 * inter-frame state instead of silent corruption).
 *
 * Frame durations arrive as lc3_frame_duration_t — hundredths of a
 * millisecond (750 = 7.5 ms, 1000 = 10 ms) — and liblc3 wants
 * microseconds, hence the x10.
 */

/*
 * Only liblc3's header is included.  Alif's vendor/include/lc3_api.h
 * cannot be: it declares `lc3_encoder_t` / `lc3_decoder_t` as opaque
 * byte-array structs while liblc3 uses the same names for its context
 * pointers.  The Alif side of the ABI is therefore restated here — the
 * entry points take the caller's structs as void pointers (identical
 * calling convention) and the sizes are asserted against the numbers
 * lc3_api.h fixes.
 */
#include "stub.h"

#include "lc3.h"

/* vendor/include/lc3_api.h */
#define ALIF_LC3_CFG_STRUCT_SIZE     2012
#define ALIF_LC3_ENCODER_STRUCT_SIZE 1576
#define ALIF_LC3_DECODER_STRUCT_SIZE 132
#define ALIF_LC3_INIT_ERR_NONE       0
/* lc3_frame_duration_t is hundredths of a millisecond: 750 = 7.5 ms,
 * 1000 = 10 ms.  liblc3 wants microseconds. */
#define ALIF_DURATION_TO_US(d)       ((int)(d) * 10)

#define LC3_MAGIC_CFG 0x4C433343u /* "LC3C" */
#define LC3_MAGIC_ENC 0x4C433345u /* "LC3E" */
#define LC3_MAGIC_DEC 0x4C433344u /* "LC3D" */
#define LC3_MAGIC_MEM 0x4C43334Du /* "LC3M" */

/* Header stamped on the caller's scratch/status buffer so we can tell a
 * buffer that already holds our context from a fresh (or recycled) one.
 * Sized to keep the liblc3 context pointer-aligned. */
struct lc3_mem_hdr {
    uint32_t magic;
    const void *owner;   /* the encoder/decoder instance it belongs to */
    uint32_t reserved[2];
};

struct lc3_cfg_hdr {
    uint32_t magic;
    int32_t fs;
    int32_t dt_us;
};

struct lc3_enc_hdr {
    uint32_t magic;
    int32_t fs;
    int32_t dt_us;
};

struct lc3_dec_hdr {
    uint32_t magic;
    int32_t fs;
    int32_t dt_us;
    lc3_decoder_t dec;   /* inside the caller's status_mem */
};

/* The opaque structs must be able to hold our headers. */
_Static_assert(sizeof(struct lc3_cfg_hdr) <= ALIF_LC3_CFG_STRUCT_SIZE, "cfg");
_Static_assert(sizeof(struct lc3_enc_hdr) <= ALIF_LC3_ENCODER_STRUCT_SIZE,
               "enc");
_Static_assert(sizeof(struct lc3_dec_hdr) <= ALIF_LC3_DECODER_STRUCT_SIZE,
               "dec");

static struct lc3_cfg_hdr *cfg_hdr(void *cfg)
{
    return (struct lc3_cfg_hdr *)cfg;
}

static bool cfg_valid(void *cfg)
{
    return cfg && cfg_hdr(cfg)->magic == LC3_MAGIC_CFG;
}

static void *mem_body(void *mem)
{
    return (uint8_t *)mem + sizeof(struct lc3_mem_hdr);
}

/*
 * lc3_api_rom_init(): on hardware this loads the ROM patch table.  There
 * is no ROM to patch here and the firmware never calls it on the boot
 * path we emulate, but it is a pinned symbol and audio_stream's codec
 * init reaches it, so report plain success.
 */
int hstub_lc3_api_rom_init(void const *patch)
{
    (void)patch;
    return ALIF_LC3_INIT_ERR_NONE;
}

int hstub_lc3_api_configure(void *cfg, int32_t fs, int32_t duration)
{
    struct lc3_cfg_hdr *h;
    int dt_us = ALIF_DURATION_TO_US(duration);

    if (!cfg) {
        return -1;
    }
    /* Reject anything liblc3 would refuse later, while the caller can
     * still see an error code. */
    if (lc3_frame_samples(dt_us, fs) < 0) {
        return -1;
    }

    h = cfg_hdr(cfg);
    h->magic = LC3_MAGIC_CFG;
    h->fs = fs;
    h->dt_us = dt_us;
    return 0;
}

uint16_t hstub_lc3_api_get_byte_count(uint32_t bitrate, int fs,
                                      int32_t duration)
{
    int bytes;

    (void)fs; /* the frame size depends only on duration and bitrate */
    bytes = lc3_frame_bytes(ALIF_DURATION_TO_US(duration), (int)bitrate);
    return bytes < 0 ? 0 : (uint16_t)bytes;
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

size_t hstub_lc3_api_encoder_scratch_size(void *cfg)
{
    struct lc3_cfg_hdr *h = cfg_hdr(cfg);

    if (!cfg_valid(cfg)) {
        return 0;
    }
    /* This buffer is where the liblc3 encoder itself lives. */
    return sizeof(struct lc3_mem_hdr) +
           lc3_encoder_size(h->dt_us, h->fs);
}

int hstub_lc3_api_initialise_encoder(void *cfg, void *encoder)
{
    struct lc3_enc_hdr *e;

    if (!cfg_valid(cfg) || !encoder) {
        return -1;
    }
    e = (struct lc3_enc_hdr *)encoder;
    e->magic = LC3_MAGIC_ENC;
    e->fs = cfg_hdr(cfg)->fs;
    e->dt_us = cfg_hdr(cfg)->dt_us;
    /* The liblc3 context is set up on the first encode_frame(), the
     * first call that sees scratch_mem. */
    return 0;
}

int hstub_lc3_api_encode_frame(void *cfg, void *encoder,
                               int16_t *input, uint8_t *output,
                               uint16_t output_len, int32_t *scratch_mem)
{
    struct lc3_enc_hdr *e = (struct lc3_enc_hdr *)encoder;
    struct lc3_mem_hdr *m = (struct lc3_mem_hdr *)scratch_mem;
    lc3_encoder_t enc;

    if (!cfg_valid(cfg) || !encoder || !input || !output || !scratch_mem ||
        e->magic != LC3_MAGIC_ENC) {
        return -1;
    }

    if (m->magic != LC3_MAGIC_MEM || m->owner != encoder) {
        /* Fresh buffer, or one another encoder was using: (re)build the
         * context here.  Only inter-frame state is lost. */
        enc = lc3_setup_encoder(e->dt_us, e->fs, 0, mem_body(m));
        if (!enc) {
            return -1;
        }
        m->magic = LC3_MAGIC_MEM;
        m->owner = encoder;
    } else {
        enc = mem_body(m);
    }

    return lc3_encode(enc, LC3_PCM_FORMAT_S16, input, 1, output_len, output);
}

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

size_t hstub_lc3_api_decoder_status_size(void *cfg)
{
    struct lc3_cfg_hdr *h = cfg_hdr(cfg);

    if (!cfg_valid(cfg)) {
        return 0;
    }
    /* This buffer is where the liblc3 decoder itself lives. */
    return sizeof(struct lc3_mem_hdr) +
           lc3_decoder_size(h->dt_us, h->fs);
}

size_t hstub_lc3_api_decoder_scratch_size(void *cfg)
{
    /* liblc3 keeps its temporaries inside the decoder context, so no
     * scratch is needed — but the caller allocates whatever we return
     * and treats a failed allocation as fatal, so never say zero. */
    (void)cfg;
    return sizeof(struct lc3_mem_hdr);
}

int hstub_lc3_api_initialise_decoder(void *cfg, void *decoder,
                                     int32_t *status_mem)
{
    struct lc3_dec_hdr *d;
    struct lc3_mem_hdr *m = (struct lc3_mem_hdr *)status_mem;
    lc3_decoder_t dec;

    if (!cfg_valid(cfg) || !decoder || !status_mem) {
        return -1;
    }
    dec = lc3_setup_decoder(cfg_hdr(cfg)->dt_us, cfg_hdr(cfg)->fs, 0,
                            mem_body(m));
    if (!dec) {
        return -1;
    }
    m->magic = LC3_MAGIC_MEM;
    m->owner = decoder;

    d = (struct lc3_dec_hdr *)decoder;
    d->magic = LC3_MAGIC_DEC;
    d->fs = cfg_hdr(cfg)->fs;
    d->dt_us = cfg_hdr(cfg)->dt_us;
    d->dec = dec;
    return 0;
}

int hstub_lc3_api_decode_frame(void *cfg, void *decoder,
                               const uint8_t *input, int input_len,
                               uint8_t bad_frame, uint8_t *bec_detect,
                               int16_t *output, int32_t *scratch_mem)
{
    struct lc3_dec_hdr *d = (struct lc3_dec_hdr *)decoder;
    int ret;

    (void)scratch_mem;

    if (bec_detect) {
        *bec_detect = 0;
    }
    if (!cfg_valid(cfg) || !decoder || !output ||
        d->magic != LC3_MAGIC_DEC) {
        return -1;
    }

    /* A bad/lost frame is signalled to liblc3 by passing no bitstream,
     * which runs its packet-loss concealment. */
    ret = lc3_decode(d->dec, bad_frame ? NULL : input, input_len,
                     LC3_PCM_FORMAT_S16, output, 1);
    if (ret < 0) {
        return -1;
    }
    if (ret == 1 && bec_detect) {
        *bec_detect = 1;
    }
    return 0;
}
