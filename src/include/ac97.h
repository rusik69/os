#ifndef AC97_H
#define AC97_H

#include "types.h"
#include "sound_pcm.h"

/* ── NAM (Native Audio Mixer) register offsets ────────────────────── */
/* All are 16-bit registers at even byte offsets 0x00–0x7E in I/O space. */

/* Reset & identification */
#define AC97_REG_RESET           0x00   /* Reset / ID (write 1 to reset) */

/* Master/summing stereo registers */
#define AC97_REG_MASTER          0x02   /* Master volume (5-bit L/R, mute) */
#define AC97_REG_AUX_OUT         0x04   /* Aux out / headphone (6-bit L/R, mute) */
#define AC97_REG_MASTER_MONO     0x06   /* Master mono volume (5-bit, mute) */
#define AC97_REG_MASTER_TONE_L   0x08   /* Master tone (left) — bass[4:0] */
#define AC97_REG_MASTER_TONE_R   0x0A   /* Master tone (right) — treble[4:0] */

/* Per-source stereo volume registers (5-bit L + R + mute bit 15) */
#define AC97_REG_PC_BEEP         0x0A   /* PC beep volume (6-bit vol, beep en bit 6) */
#define AC97_REG_PHONE           0x0C   /* Phone volume (5-bit, mute) */
#define AC97_REG_MIC             0x0E   /* Mic volume (5-bit, mute, 20dB boost bit 6) */
#define AC97_REG_LINE_IN         0x10   /* Line-in volume */
#define AC97_REG_CD              0x12   /* CD volume */
#define AC97_REG_VIDEO           0x14   /* Video volume */
#define AC97_REG_AUX             0x16   /* Aux volume */
#define AC97_REG_PCM_OUT         0x18   /* PCM output volume */
#define AC97_REG_RECORD_SELECT   0x1A   /* Record source select (L[7:0], R[15:8]) */
#define AC97_REG_RECORD_GAIN     0x1C   /* Record gain (4-bit L/R, mute) */
#define AC97_REG_RECORD_SOURCE   0x1E   /* Record source (L[7:0], R[15:8] — alt) */

/* 3D enhancement */
#define AC97_REG_3D_CONTROL      0x22   /* 3D sound control */
#define AC97_REG_3D_SELECT       0x26   /* 3D source select */

/* Extended audio / sample rate */
#define AC97_REG_POWERDOWN       0x26   /* Powerdown control/status */
#define AC97_REG_EXT_AUDIO_ID    0x28   /* Extended audio ID (read-only) */
#define AC97_REG_EXT_AUDIO_CTRL  0x2A   /* Extended audio control */
#define AC97_REG_FRONT_RATE      0x2C   /* PCM front DAC sample rate */
#define AC97_REG_SURROUND_RATE   0x2E   /* PCM surround DAC rate */
#define AC97_REG_LFE_RATE        0x30   /* PCM LFE DAC rate */
#define AC97_REG_PCM_ADC_RATE    0x32   /* PCM ADC sample rate */
#define AC97_REG_MIC_ADC_RATE    0x34   /* Mic ADC rate */
#define AC97_REG_SPDIF_CONTROL   0x36   /* SPDIF control */

/* ── Powerdown register (AC97_REG_POWERDOWN, 0x26) bit definitions ── */
#define AC97_PD_PR0       (1U << 0)  /* ADC powerdown */
#define AC97_PD_PR1       (1U << 1)  /* DAC powerdown */
#define AC97_PD_PR2       (1U << 2)  /* Analog mixer powerdown */
#define AC97_PD_PR3       (1U << 3)  /* AC-Link powerdown */
#define AC97_PD_PR4       (1U << 4)  /* Internal clocks disable */
#define AC97_PD_PR5       (1U << 5)  /* Wakeup status (read-only) */
#define AC97_PD_PR6       (1U << 6)  /* VREF powerdown */
#define AC97_PD_PR7       (1U << 7)  /* Reserved, must be 0 */
#define AC97_PD_EAPD      (1U << 15) /* External amplifier power down */

/* All audio functions powered down (deep sleep) */
#define AC97_PD_ALL       (AC97_PD_PR0 | AC97_PD_PR1 | AC97_PD_PR2 | AC97_PD_PR3 | AC97_PD_PR4 | AC97_PD_PR6)

/* NABM Global Control register bits */
#define AC97_GC_COLD_RESET  (1U << 1)  /* Cold reset AC-link */
#define AC97_GC_WARM_RESET  (1U << 2)  /* Warm reset AC-link */
#define AC97_GC_PCM_SLOT_MAP 0x00000001U /* PCM slot map enable */

/* AC97 power management states */
enum ac97_power_state {
    AC97_POWER_D0 = 0,   /**< Fully on: all functions active */
    AC97_POWER_D1,        /**< Low-power: mixer active, DAC/ADC off */
    AC97_POWER_D2,        /**< Deep sleep: only wakeup logic active */
    AC97_POWER_D3,        /**< Off: AC-Link powered down, requires cold reset */
    AC97_POWER_D3_COLD,   /**< Full power-off (cold reset required) */
};

/* Misc */
#define AC97_REG_MISC            0x3C   /* Misc volume/3D */
#define AC97_REG_VENDOR_ID1      0x7C   /* Vendor ID1 (high 16 bits) */
#define AC97_REG_VENDOR_ID2      0x7E   /* Vendor ID2 (low 16 bits) */

/* ── AC97 mixer channel identifiers ──────────────────────────────── */

enum ac97_mixer_channel {
    AC97_MIXER_MASTER     = 0,   /**< Master output volume */
    AC97_MIXER_AUX_OUT,          /**< Aux out / headphone */
    AC97_MIXER_MASTER_MONO,      /**< Master mono output */
    AC97_MIXER_PC_BEEP,          /**< PC beep / speaker */
    AC97_MIXER_PHONE,            /**< Phone input/volume */
    AC97_MIXER_MIC,              /**< Microphone volume */
    AC97_MIXER_LINE_IN,          /**< Line input volume */
    AC97_MIXER_CD,               /**< CD audio input */
    AC97_MIXER_VIDEO,            /**< Video input volume */
    AC97_MIXER_AUX,              /**< Aux input volume */
    AC97_MIXER_PCM,              /**< PCM output volume */
    AC97_MIXER_RECORD_GAIN,      /**< Recording gain */
    AC97_MIXER_CHANNEL_COUNT     /**< Number of mixer channels */
};

/* ── Volume-related constants ─────────────────────────────────────── */

/** Gain in percentage: 0 = min (silent), 100 = max (0dB/unity) */
#define AC97_VOLUME_MIN    0U
#define AC97_VOLUME_MAX    100U
#define AC97_VOLUME_DEFAULT 75U

/* ── Initialize AC'97 audio controller (PCI class 04:01).
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

/**
 * ac97_reset — Perform a cold reset of the AC97 codec.
 * Returns 0 on success, or a negative errno on failure.
 */
int ac97_reset(void);

/* ── Power Management API ─────────────────────────────────────────── */

/**
 * ac97_cold_reset — Full AC-link cold reset.
 *
 * Asserts the cold reset bit in NABM Global Control for 1us, releases it,
 * then waits for the codec to stabilise and re-reads vendor ID to confirm.
 *
 * Returns 0 on success, -ENODEV if AC97 is not present, -EIO if the
 * codec does not respond after reset.
 */
int ac97_cold_reset(void);

/**
 * ac97_warm_reset — Warm reset of the AC-link.
 *
 * Toggles the warm reset bit in NABM Global Control to resume the AC-link
 * from PR3 (AC-Link powerdown) without a full cold reset.
 * After warm reset, the codec retains mixer settings and register state.
 *
 * Returns 0 on success, -ENODEV if AC97 is not present.
 */
int ac97_warm_reset(void);

/**
 * ac97_suspend — Suspend the AC97 device to a given power state.
 *
 * @state: Target power state (AC97_POWER_D1, D2, D3, or D3_COLD).
 *
 * D1: Powers down ADC only (recording off).
 * D2: Powers down ADC, DAC, and analog mixer (playback + recording off).
 * D3: Powers down AC-Link (requires warm resume or cold reset).
 * D3_COLD: Full power-off (requires cold reset to resume).
 *
 * Returns 0 on success, -ENODEV if AC97 is not present, -EINVAL for
 * unsupported state requests.
 */
int ac97_suspend(enum ac97_power_state state);

/**
 * ac97_resume — Resume the AC97 device from a suspended state.
 *
 * Selects the appropriate resume method based on current power state:
 *   D3_COLD -> full cold reset + re-initialisation
 *   D3      -> warm reset + restore mixer state
 *   D1/D2   -> clear powerdown bits
 *
 * Returns 0 on success, -ENODEV if AC97 is not present, -EIO on resume
 * failure.
 */
int ac97_resume(void);

/**
 * ac97_get_power_state — Return the current AC97 power state.
 */
enum ac97_power_state ac97_get_power_state(void);

/**
 * ac97_set_amplifier_power — Control external amplifier power.
 *
 * @on: 1 to power up amplifier, 0 to power down.
 *
 * Uses the EAPD bit in the powerdown register (AC97_REG_POWERDOWN bit 15).
 * May not be supported on all codecs.
 *
 * Returns 0 on success, -ENODEV if AC97 is not present.
 */
int ac97_set_amplifier_power(int on);

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

/* ── Mixer control — AC97 codec mixer API ─────────────────────────── */

/** Bit masks for AC97 volume registers */
#define AC97_VOL_LEFT_MASK      0x001F   /* bits 4:0 = left channel volume */
#define AC97_VOL_RIGHT_MASK     0x1F00   /* bits 12:8 = right channel volume */
#define AC97_VOL_MUTE_BIT       0x8000   /* bit 15 = mute */
#define AC97_MIC_20DB_BIT       0x0040   /* bit 6 = mic 20dB boost */
#define AC97_BEEP_ENABLE_BIT    0x0040   /* bit 6 = PC beep enable */
#define AC97_MASTER_MUTE_BIT    0x8000   /* bit 15 = master mute */

/**
 * ac97_mixer_set_volume — Set a mixer channel volume in 0–100%.
 *
 * @ch:   AC97 mixer channel identifier.
 * @left: Left volume (0 = silent, 100 = 0dB max).
 * @right: Right volume (0 = silent, 100 = 0dB max).
 *
 * Converts the 0–100 range to AC97 5-bit (0=max/31=min) register values
 * and writes to the appropriate NAM register.  Unmutes the channel if it
 * was muted (unless both volumes are 0).
 *
 * Returns 0 on success, -EINVAL if @ch is out of range, -ENODEV if AC97
 * is not present.
 */
int ac97_mixer_set_volume(enum ac97_mixer_channel ch, uint8_t left, uint8_t right);

/**
 * ac97_mixer_get_volume — Read a mixer channel volume in 0–100%.
 *
 * @ch:    AC97 mixer channel identifier.
 * @left:  Receives left volume (0–100), may be NULL.
 * @right: Receives right volume (0–100), may be NULL.
 *
 * Returns 0 on success, -EINVAL if @ch is out of range, -ENODEV if AC97
 * is not present.
 */
int ac97_mixer_get_volume(enum ac97_mixer_channel ch, uint8_t *left, uint8_t *right);

/**
 * ac97_mixer_set_mute — Mute or unmute a mixer channel.
 *
 * @ch:   AC97 mixer channel identifier.
 * @mute: 1 to mute, 0 to unmute.
 *
 * Returns 0 on success, -EINVAL if @ch is out of range, -ENODEV if AC97
 * is not present.
 */
int ac97_mixer_set_mute(enum ac97_mixer_channel ch, int mute);

/**
 * ac97_mixer_get_mute — Read the mute state of a mixer channel.
 *
 * @ch:   AC97 mixer channel identifier.
 * @mute: Receives 1 if muted, 0 if unmuted.
 *
 * Returns 0 on success, -EINVAL if @ch is out of range, -ENODEV if AC97
 * is not present.
 */
int ac97_mixer_get_mute(enum ac97_mixer_channel ch, int *mute);

/**
 * ac97_mixer_set_mic_boost — Enable/disable microphone 20dB boost.
 *
 * @enable: 1 to enable boost, 0 to disable.
 *
 * The 20dB boost is a hardware pre-amplifier for the microphone input.
 * Only meaningful on the MIC channel.
 *
 * Returns 0 on success, -ENODEV if AC97 is not present.
 */
int ac97_mixer_set_mic_boost(int enable);

/**
 * ac97_mixer_get_mic_boost — Read microphone 20dB boost state.
 *
 * @enabled: Receives 1 if boost is enabled, 0 if disabled.
 *
 * Returns 0 on success, -ENODEV if AC97 is not present.
 */
int ac97_mixer_get_mic_boost(int *enabled);

/**
 * ac97_mixer_set_tone — Set bass/treble tone control.
 *
 * @bass:   Bass level (0–15, AC97 tone control range).
 * @treble: Treble level (0–15).
 *
 * Both 0 = flat response.  Only affects codecs that implement
 * the tone control registers (AC97_REG_MASTER_TONE_L/R).
 *
 * Returns 0 on success, -ENODEV if AC97 is not present.
 */
int ac97_mixer_set_tone(uint8_t bass, uint8_t treble);

/**
 * ac97_mixer_set_record_gain — Set recording gain with percentage API.
 *
 * @ch:    Recording source channel (AC97_MIXER_LINE_IN, AC97_MIXER_MIC, etc.).
 * @left:  Left channel gain in 0–100% (mapped to 0–15 AC97 gain steps).
 * @right: Right channel gain in 0–100%.
 *
 * Returns 0 on success, -EINVAL if @ch is out of range, -ENODEV if AC97
 * is not present.
 */
int ac97_mixer_set_record_gain(enum ac97_mixer_channel ch, uint8_t left, uint8_t right, int mute);

/**
 * ac97_mixer_init_defaults — Set all mixer channels to safe defaults.
 *
 * Initialises all channels to 75% volume, unmuted, with mic boost off
 * and recording source set to microphone.  Should be called after
 * ac97_init().
 */
void ac97_mixer_init_defaults(void);

/**
 * ac97_mixer_get_channel_name — Return a human-readable name for a channel.
 *
 * @ch: AC97 mixer channel identifier.
 *
 * Returns a static string (e.g. "Master", "PCM", "Mic"), or "Unknown"
 * for out-of-range channels.
 */
const char *ac97_mixer_get_channel_name(enum ac97_mixer_channel ch);

/**
 * ac97_mixer_get_channel_register — Return the NAM register offset for a channel.
 *
 * @ch: AC97 mixer channel identifier.
 *
 * Returns the NAM register offset (e.g. 0x02 for MASTER), or 0 if the
 * channel has no direct register mapping.
 */
uint16_t ac97_mixer_get_channel_register(enum ac97_mixer_channel ch);

/**
 * ac97_mixer_count — Return the number of available mixer channels.
 *
 * Returns AC97_MIXER_CHANNEL_COUNT.
 */
static inline int ac97_mixer_count(void) { return AC97_MIXER_CHANNEL_COUNT; }

/* ── Backward-compatible aliases ─────────────────────────────────--- */

#define AC97_MIXER_MASTER_VOL  AC97_REG_MASTER
#define AC97_MIXER_PCM_VOL     AC97_REG_PCM_OUT
#define AC97_MIXER_MIC_VOL     AC97_REG_MIC
#define AC97_MIXER_LINE_IN_REG AC97_REG_LINE_IN
#define AC97_MIXER_CD_REG      AC97_REG_CD

/**
 * Legacy: ac97_set_volume — Set raw volume for a NAM register.
 * @channel:  NAM register offset.
 * @left:     Left channel volume (0 = max, 31 = min).
 * @right:    Right channel volume (0 = max, 31 = min).
 * @mute:     1 to mute the channel, 0 to unmute.
 */
void ac97_set_volume(uint16_t channel, uint8_t left, uint8_t right, int mute);

/**
 * Legacy: ac97_get_volume — Read raw volume from a NAM register.
 */
void ac97_get_volume(uint16_t channel, uint8_t *left, uint8_t *right, int *mute);

#endif
