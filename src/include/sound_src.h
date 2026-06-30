/*
 * sound_src.h — Sample rate converter (44.1k ↔ 48k and arbitrary rates)
 *
 * Implements a phase-accumulator-based sample rate converter with
 * configurable interpolation quality.  Supports any input/output rate
 * pair, not just the 44.1k ↔ 48k common case.
 *
 * The converter uses fixed-point arithmetic (16.16) for the phase
 * accumulator and supports 16-bit signed PCM with 1 or 2 channels.
 *
 * Key design points:
 *   - Phase accumulator tracks fractional position in the input stream.
 *   - Linear interpolation blends adjacent samples for decent quality.
 *   - Nearest-neighbour mode available for maximum speed.
 *   - Zero-startup transient — reset() clears the history.
 *   - Channel-interleaved 16-bit stereo format throughout.
 *
 * Item 375 — Sample rate conversion (D142 task 4)
 */
#ifndef SOUND_SRC_H
#define SOUND_SRC_H

#include "types.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/** Maximum supported channels */
#define SOUND_SRC_MAX_CHANNELS  2U

/** Fixed-point fractional bits (16.16) */
#define SRC_FRAC_BITS          16U
#define SRC_FRAC_UNIT          (1U << SRC_FRAC_BITS)
#define SRC_FRAC_MASK          (SRC_FRAC_UNIT - 1U)

/* ── Quality modes ────────────────────────────────────────────────── */

/** SRC interpolation quality */
enum sound_src_quality {
    SOUND_SRC_NEAREST = 0,  /**< Nearest-neighbour (fastest, lowest quality) */
    SOUND_SRC_LINEAR  = 1,  /**< Linear interpolation (decent quality) */
};

/* ── SRC state ─────────────────────────────────────────────────────── */

/** Per-instance sample rate converter state */
struct sound_src_state {
    /* Configuration (set at init time) */
    uint32_t    input_rate;         /**< Source sample rate in Hz */
    uint32_t    output_rate;        /**< Target sample rate in Hz */
    int         channels;           /**< 1 = mono, 2 = stereo */
    enum sound_src_quality quality; /**< Interpolation quality */

    /* Phase accumulator (16.16 fixed point) */
    uint32_t    phase;              /**< Current fractional position */
    uint32_t    step;               /**< Phase increment per output frame */

    /* Previous frame for interpolation */
    int16_t     last_frame[SOUND_SRC_MAX_CHANNELS]; /**< Last input frame */
    int         last_valid;         /**< 1 if last_frame has valid data */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * sound_src_init — Initialise a sample rate converter.
 *
 * @src:          SRC state structure (caller-allocated).
 * @input_rate:   Source sample rate in Hz (e.g. 44100).
 * @output_rate:  Target sample rate in Hz (e.g. 48000).
 * @channels:     Number of channels (1 = mono, 2 = stereo).
 * @quality:      Interpolation quality (SOUND_SRC_LINEAR or _NEAREST).
 *
 * Returns 0 on success, or -EINVAL on invalid parameters.
 */
int sound_src_init(struct sound_src_state *src,
                   uint32_t input_rate,
                   uint32_t output_rate,
                   int channels,
                   enum sound_src_quality quality);

/**
 * sound_src_process — Convert a block of PCM samples.
 *
 * @src:            Initialised SRC state.
 * @input:          Input PCM buffer (16-bit signed, interleaved).
 * @input_frames:   Number of input frames (stereo: 2 samples per frame).
 * @output:         Output PCM buffer (caller-allocated, must be large
 *                  enough — see sound_src_estimate_output).
 * @max_output_frames: Capacity of output buffer in frames.
 *
 * Returns the number of output frames produced on success,
 * or a negative errno on error.
 */
int sound_src_process(struct sound_src_state *src,
                      const int16_t *input, uint32_t input_frames,
                      int16_t *output, uint32_t max_output_frames);

/**
 * sound_src_drain — Flush remaining samples from the SRC pipeline.
 *
 * @src:    Initialised SRC state.
 * @output: Output buffer for remaining samples.
 * @max_output_frames: Capacity of output buffer in frames.
 *
 * After processing all input frames, call drain to get any pending
 * output samples.  This is needed for downsampling where the last
 * input frame may contribute multiple output frames.
 *
 * Returns the number of output frames produced, or a negative errno.
 */
int sound_src_drain(struct sound_src_state *src,
                    int16_t *output, uint32_t max_output_frames);

/**
 * sound_src_reset — Reset the SRC state (clears sample history).
 *
 * @src: SRC state to reset.
 *
 * Call this when starting a new stream to avoid pop/click artefacts
 * from stale sample history.
 */
void sound_src_reset(struct sound_src_state *src);

/**
 * sound_src_estimate_output — Estimate the output frame count.
 *
 * @src:         Initialised SRC state.
 * @input_frames: Number of input frames.
 *
 * Returns an upper bound on the number of output frames that
 * sound_src_process() will produce for the given input count.
 * This is safe for sizing the output buffer.
 */
uint32_t sound_src_estimate_output(const struct sound_src_state *src,
                                   uint32_t input_frames);

/**
 * sound_src_convert_rate — One-shot rate conversion convenience.
 *
 * @input:          Input PCM buffer (16-bit signed, interleaved).
 * @input_frames:   Number of input frames.
 * @channels:       Number of channels (1 or 2).
 * @input_rate:     Source sample rate.
 * @output_rate:    Target sample rate.
 * @output:         Output buffer (caller-allocated).
 * @max_output_frames: Capacity of output buffer.
 *
 * Convenience wrapper that creates a temporary SRC state, processes
 * all frames, drains, and returns the total output frame count.
 * Uses linear interpolation.
 *
 * Returns the number of output frames, or negative errno.
 */
int sound_src_convert_rate(const int16_t *input, uint32_t input_frames,
                           int channels,
                           uint32_t input_rate, uint32_t output_rate,
                           int16_t *output, uint32_t max_output_frames);

#endif /* SOUND_SRC_H */
