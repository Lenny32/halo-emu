/*
 * Halo — minimal 16-bit-PCM RIFF/WAVE reader and writer shared by the
 * audio models (halo_i2s.c speaker sink, halo_pdm.c microphone source).
 *
 * Deliberately tiny: the only formats on the device's audio paths are
 * mono/stereo signed 16-bit LE at 8/16/32 kHz, so anything else is
 * rejected at open time rather than converted.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HALO_WAV_H
#define HW_ARM_HALO_WAV_H

#include "qapi/error.h"

typedef struct HaloWavWriter HaloWavWriter;
typedef struct HaloWavReader HaloWavReader;

/*
 * Writer.  The header is (re)written on every flush, so the file on
 * disk is a valid WAV at all times — a killed QEMU loses at most the
 * samples still in the buffer, and a test can read the capture back
 * without shutting the emulator down.
 */
HaloWavWriter *halo_wav_writer_open(const char *path, int rate,
                                    int channels, Error **errp);
void halo_wav_writer_write(HaloWavWriter *w, const int16_t *samples,
                           size_t nsamples);
/* Push the buffer out and refresh the RIFF/data lengths. */
void halo_wav_writer_flush(HaloWavWriter *w);
void halo_wav_writer_close(HaloWavWriter *w);
/* Total frames written so far (samples / channels). */
uint32_t halo_wav_writer_frames(const HaloWavWriter *w);
/*
 * The writer is opened at the rate the guest had configured at the
 * time; if the guest reconfigures mid-file we keep the original rate
 * (a WAV has only one) and say so once.
 */
int halo_wav_writer_rate(const HaloWavWriter *w);

/*
 * Reader.  The whole file is slurped at open (device audio clips are
 * small); reads loop back to the start at EOF so a short clip becomes
 * a continuous signal.
 */
HaloWavReader *halo_wav_reader_open(const char *path, Error **errp);
int halo_wav_reader_rate(const HaloWavReader *r);
int halo_wav_reader_channels(const HaloWavReader *r);
/*
 * Fill @out with @nsamples mono samples, downmixing stereo and
 * resampling (nearest-neighbour) from the file's rate to @rate.
 * An empty file yields silence.
 */
void halo_wav_reader_read(HaloWavReader *r, int rate, int16_t *out,
                          size_t nsamples);
void halo_wav_reader_close(HaloWavReader *r);

#endif /* HW_ARM_HALO_WAV_H */
