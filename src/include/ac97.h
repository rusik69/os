#ifndef AC97_H
#define AC97_H

#include "types.h"
#include "sound_pcm.h"

/* Initialize AC'97 audio controller (PCI class 04:01).
 * Returns 0 if found and initialized, -1 if absent. */
int ac97_init(void);

/* Play a buffer of 16-bit signed stereo PCM samples.
 * rate: sample rate in Hz (e.g. 44100).
 * len:  number of BYTES in samples[]. */
void ac97_play_pcm(const int16_t *samples, uint32_t len, uint32_t rate);

/* Returns 1 if AC'97 device is present. */
int ac97_present(void);

/* ── Codec register access ────────────────────────────────────────── */

/**
 * ac97_read — Read an AC97 NAM register.
 * @reg: Register offset (0x00--0x7E, must be even).
 * Returns the 16-bit register value, or a negative errno on error.
 */
int ac97_read(int reg);

/**
 * ac97_write — Write an AC97 NAM register.
 * @reg: Register offset (0x00--0x7E, must be even).
 * @val: 16-bit value to write.
 * Returns 0 on success, or a negative errno on error.
 */
int ac97_write(uint16_t reg, uint16_t val);

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

/* ── Interrupt-driven playback API ──────────────────────────────────── */

/** Maximum number of BDL entries for playback */
#define AC97_BDL_ENTRIES 32

/**
 * ac97_playback_start — Start interrupt-driven PCM playback.
 *
 * @stream:  Initialised PCM playback stream (from sound_pcm_init_stream).
 *
 * Fills the BDL ring from the PCM stream and starts the DMA engine
 * with IOC interrupts enabled.  Returns immediately (non-blocking).
 * Subsequent DMA completion interrupts refill BDL entries from the
 * PCM stream automatically.
 *
 * Returns 0 on success, -EINVAL/ -EBUSY/ -ENODATA on failure.
 */
int ac97_playback_start(struct sound_pcm_stream *stream);

/**
 * ac97_playback_stop — Stop interrupt-driven PCM playback.
 *
 * Halts the DMA engine and clears status bits.  The PCM stream
 * is left intact and may be restarted later.
 */
void ac97_playback_stop(void);

/**
 * ac97_playback_is_active — Check if DMA playback is running.
 *
 * Returns 1 if the DMA engine is actively processing playback BDL entries.
 */
int ac97_playback_is_active(void);

/* ── Interrupt-driven capture API ───────────────────────────────────── */


/**
 * ac97_capture_start — Start interrupt-driven PCM capture.
 *
 * @stream:  Initialised PCM capture stream (from sound_pcm_init_stream
 *           with SOUND_PCM_CAPTURE direction).
 *
 * Fills the capture BDL ring with empty buffers and starts the DMA engine
 * with IOC interrupts enabled.  Returns immediately (non-blocking).
 * Subsequent DMA completion interrupts copy captured data from the BDL
 * buffers into the PCM stream and refill empty BDL entries automatically.
 *
 * Returns 0 on success, -EINVAL/ -EBUSY/ -ENODATA on failure.
 */
int ac97_capture_start(struct sound_pcm_stream *stream);

/**
 * ac97_capture_stop — Stop interrupt-driven PCM capture.
 *
 * Halts the capture DMA engine and clears status bits.  The PCM stream
 * is left intact and may be restarted later.  Any partially-captured
 * data remains in the PCM buffer for the application to read.
 */
void ac97_capture_stop(void);

/**
 * ac97_capture_is_active — Check if DMA capture is running.
 *
 * Returns 1 if the DMA engine is actively capturing into BDL entries.
 */
int ac97_capture_is_active(void);

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
