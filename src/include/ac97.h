#ifndef AC97_H
#define AC97_H

#include "types.h"

/* Initialize AC'97 audio controller (PCI class 04:01).
 * Returns 0 if found and initialized, -1 if absent. */
int ac97_init(void);

/* Play a buffer of 16-bit signed stereo PCM samples.
 * rate: sample rate in Hz (e.g. 44100).
 * len:  number of BYTES in samples[]. */
void ac97_play_pcm(const int16_t *samples, uint32_t len, uint32_t rate);

/* Returns 1 if AC'97 device is present. */
int ac97_present(void);

/* ── Capture (Recording) API ─────────────────────────────────────── */

/**
 * ac97_capture_read — Capture audio samples from the selected input source.
 *
 * Performs a single-shot DMA capture, reading samples from the currently
 * selected record source (microphone, line-in, etc.) into the provided buffer.
 *
 * @buf:   16-bit signed PCM output buffer (must hold at least @bytes).
 * @bytes: Number of bytes to capture (should be even for 16-bit samples).
 * @rate:  Sample rate in Hz (8000–48000, default 44100).
 *
 * Returns the number of bytes captured on success, or -1 on error.
 * On timeout (no audio source connected), returns 0.
 */
int ac97_capture_read(int16_t *buf, uint32_t bytes, uint32_t rate);

/**
 * ac97_set_record_source — Select recording input source.
 * @source: One of REC_SEL_MIC, REC_SEL_CD, REC_SEL_LINE_IN, etc.
 */
void ac97_set_record_source(uint16_t source);

/**
 * ac97_set_record_gain — Set recording gain level.
 * @left:   Left channel gain (0–15, 0 = 0dB, 15 = +22.5dB).
 * @right:  Right channel gain (0–15).
 * @mute:   1 to mute recording, 0 to unmute.
 */
void ac97_set_record_gain(uint8_t left, uint8_t right, int mute);

/* Record source select constants (for ac97_set_record_source) */
#define REC_SEL_MIC     0x0000
#define REC_SEL_CD      0x0101
#define REC_SEL_VIDEO   0x0202
#define REC_SEL_AUX     0x0303
#define REC_SEL_LINE_IN 0x0404
#define REC_SEL_STEREO  0x0505
#define REC_SEL_MONO    0x0606
#define REC_SEL_PHONE   0x0707

/* ── Mixer control ───────────────────────────────────────────────── */

/** AC'97 mixer channels (NAM register indices) */
#define AC97_MIXER_MASTER   0x02   /* Master volume */
#define AC97_MIXER_PCM      0x18   /* PCM output volume */
#define AC97_MIXER_MIC      0x0E   /* Microphone volume */
#define AC97_MIXER_LINE_IN  0x10   /* Line input volume */
#define AC97_MIXER_CD       0x12   /* CD audio volume */

/**
 * ac97_set_volume — Set playback volume for a given mixer channel.
 * @channel:  NAM register offset (e.g. AC97_MIXER_MASTER, AC97_MIXER_PCM).
 * @left:     Left channel volume (0 = max, 31 = min, per AC97 spec).
 * @right:    Right channel volume (0 = max, 31 = min).
 * @mute:     1 to mute the channel, 0 to unmute.
 *
 * Writes the combined volume+mute value to the NAM register.
 * If mute is 1, the mute bit (bit 15) is OR'd into the written value.
 */
void ac97_set_volume(uint16_t channel, uint8_t left, uint8_t right, int mute);

/**
 * ac97_get_volume — Read current playback volume for a mixer channel.
 * @channel:  NAM register offset.
 * @left:     Receives left channel volume (0 = max, 31 = min).
 * @right:    Receives right channel volume (0 = max, 31 = min).
 * @mute:     Receives 1 if muted, 0 otherwise.
 */
void ac97_get_volume(uint16_t channel, uint8_t *left, uint8_t *right, int *mute);

#endif
