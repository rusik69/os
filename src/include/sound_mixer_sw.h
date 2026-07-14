/*
 * sound_mixer_sw.h — Software mixer: multi-stream PCM mixing
 *
 * Provides a software audio mixer that accepts multiple concurrent
 * PCM streams, sums them with per-channel volume, and produces a
 * single mixed output for the hardware DMA engine.
 *
 * Each application opens a virtual "channel" (like dmix on Linux),
 * writes PCM frames independently, and the mixer combines them on
 * each mix cycle with saturation-safe accumulation.
 *
 * Task #13 — D142 Software mixer (multi-stream mixing)
 */
#ifndef SOUND_MIXER_SW_H
#define SOUND_MIXER_SW_H

#include "types.h"
#include "spinlock.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/** Maximum number of concurrent software mixer channels (streams) */
#define SOUND_MIXER_SW_MAX_CHANNELS    8

/** Maximum PCM channels (1 = mono, 2 = stereo) */
#define SOUND_MIXER_SW_MAX_PCM_CHANNELS  2

/** Mix accumulator buffer size in frames */
#define SOUND_MIXER_SW_BUFFER_FRAMES  4096

/** Per-channel ring buffer size in frames (must be power of 2) */
#define SOUND_MIXER_SW_CHAN_FRAMES    8192

/* ── Software mixer channel ────────────────────────────────────────── */

/** Per-channel state for the software mixer */
struct sound_mixer_sw_channel {
	int         active;          /**< 1 if this channel is in use */
	int         allocated;       /**< 1 if ring buffer is allocated */
	uint8_t     volume;          /**< Per-channel volume (0..255) */
	uint8_t     mute;            /**< 1 if muted */

	/* Ring buffer for this channel's PCM frames */
	int16_t    *buffer;          /**< Channel ring buffer (s16 interleaved) */
	uint32_t    buf_frames;      /**< Buffer capacity in frames */
	uint32_t    buf_mask;        /**< buf_frames - 1 (power of 2) */

	/* Cursors */
	volatile uint32_t  write_idx;  /**< Write cursor in frames */
	volatile uint32_t  read_idx;   /**< Read cursor in frames */

	/* Format */
	uint8_t     pcm_channels;    /**< 1 = mono, 2 = stereo */

	/* Per-channel lock (protects ring buffer cursors) */
	spinlock_t  lock;
};

/* ── Software mixer state ──────────────────────────────────────────── */

/** Top-level software mixer state */
struct sound_mixer_sw {
	/* Array of channel slots */
	struct sound_mixer_sw_channel channels[SOUND_MIXER_SW_MAX_CHANNELS];

	/* Mix accumulator buffer (temporary workspace for mixing) */
	int32_t    *mix_buffer;      /**< Accumulator for summing samples (int32_t to avoid overflow) */
	uint32_t    mix_frames;      /**< Mix buffer capacity in frames */

	/* Global output format */
	uint8_t     pcm_channels;    /**< Output PCM channels (1 or 2) */
	uint32_t    sample_rate;     /**< Output sample rate in Hz */

	/* Active channel count */
	int         active_count;

	/* Top-level lock (protects channel assignment) */
	spinlock_t  lock;
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * sound_mixer_sw_init — Initialise the software mixer.
 *
 * @mixer:        Mixer state structure (caller-allocated).
 * @pcm_channels: Number of output channels (1 or 2).
 * @sample_rate:  Output sample rate in Hz.
 *
 * Allocates the mix accumulator buffer and initialises all channel
 * slots to the inactive state.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
int sound_mixer_sw_init(struct sound_mixer_sw *mixer,
			uint8_t pcm_channels,
			uint32_t sample_rate);

/**
 * sound_mixer_sw_destroy — Destroy the software mixer and free resources.
 *
 * Frees all channel ring buffers and the mix accumulator buffer.
 */
void sound_mixer_sw_destroy(struct sound_mixer_sw *mixer);

/**
 * sound_mixer_sw_open_channel — Allocate a new virtual mixer channel.
 *
 * @mixer:  Mixer state.
 * @volume: Initial per-channel volume (0..255, 255 = max).
 *
 * Finds a free channel slot, allocates its ring buffer, and marks it
 * active.
 *
 * Returns the channel index (0 .. SOUND_MIXER_SW_MAX_CHANNELS-1)
 * on success, or a negative errno on failure.
 */
int sound_mixer_sw_open_channel(struct sound_mixer_sw *mixer, uint8_t volume);

/**
 * sound_mixer_sw_close_channel — Close and free a mixer channel.
 *
 * @mixer:  Mixer state.
 * @ch_idx: Channel index to close (from open_channel).
 */
void sound_mixer_sw_close_channel(struct sound_mixer_sw *mixer, int ch_idx);

/**
 * sound_mixer_sw_write — Write PCM frames to a mixer channel.
 *
 * @mixer:  Mixer state.
 * @ch_idx: Target channel index.
 * @data:   Interleaved 16-bit signed PCM frames.
 * @frames: Number of frames to write.
 *
 * Returns the number of frames actually written (may be less than
 * @frames if the ring buffer is full), or a negative errno.
 */
int sound_mixer_sw_write(struct sound_mixer_sw *mixer, int ch_idx,
			 const int16_t *data, uint32_t frames);

/**
 * sound_mixer_sw_mix — Mix all active channels into a single output.
 *
 * @mixer:     Mixer state.
 * @output:    Output buffer for mixed PCM (s16 interleaved).
 * @max_frames: Maximum number of frames to produce.
 *
 * Reads up to @max_frames from each active channel, sums the samples
 * with clamping to the 16-bit signed range, and writes the result to
 * @output.  Channels with no data contribute silence.
 *
 * Returns the number of frames actually produced.
 */
uint32_t sound_mixer_sw_mix(struct sound_mixer_sw *mixer,
			    int16_t *output, uint32_t max_frames);

/**
 * sound_mixer_sw_set_channel_volume — Set per-channel volume.
 *
 * @mixer:  Mixer state.
 * @ch_idx: Channel index.
 * @volume: New volume (0..255).
 */
void sound_mixer_sw_set_channel_volume(struct sound_mixer_sw *mixer,
				       int ch_idx, uint8_t volume);

/**
 * sound_mixer_sw_set_channel_mute — Mute or unmute a channel.
 *
 * @mixer:  Mixer state.
 * @ch_idx: Channel index.
 * @mute:   1 = mute (silence), 0 = unmute.
 */
void sound_mixer_sw_set_channel_mute(struct sound_mixer_sw *mixer,
				     int ch_idx, int mute);

/**
 * sound_mixer_sw_get_active_count — Return number of active channels.
 */
int sound_mixer_sw_get_active_count(struct sound_mixer_sw *mixer);

#endif /* SOUND_MIXER_SW_H */
