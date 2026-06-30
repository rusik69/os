/*
 * sound_src.c — Sample rate converter (44.1k ↔ 48k and arbitrary rates)
 *
 * Implements a phase-accumulator-based sample rate converter using
 * 16.16 fixed-point arithmetic.  Supports any input/output rate pair,
 * 16-bit signed PCM, and 1 or 2 channels.
 *
 * Algorithm:
 *   For each output frame, the phase accumulator tracks where we are
 *   in the input stream.  The integer part (phase >> 16) selects the
 *   input frame index; the fractional part is used for interpolation.
 *
 *   Linear interpolation blends adjacent frames:
 *     out = in[i] * (1 - frac) + in[i+1] * frac
 *
 *   Nearest-neighbour just takes in[i] directly (faster, lower quality).
 *
 * The phase step per output frame is:
 *   step = (input_rate << 16) / output_rate
 *
 * For the common case of 44.1k ↔ 48k, the ratio is 147:160.
 *
 * Item 375 — Sample rate conversion (D142 task 4)
 */
#include "sound_src.h"
#include "string.h"
#include "errno.h"
#include "printf.h"

/* ── Internal helpers ───────────────────────────────────────────────── */

/**
 * Compute the fixed-point phase step from input and output rates.
 * Returns the step in 16.16 format, or 0 on invalid rates.
 */
static uint32_t compute_step(uint32_t input_rate, uint32_t output_rate)
{
	if (input_rate == 0 || output_rate == 0)
		return 0;
	if (input_rate > output_rate * 16U)
		return 0; /* Too large — would overflow the accumulator */
	if (output_rate > input_rate * 16U)
		return 0; /* Too large — step would exceed 16 bits */

	/* Compute step = (input_rate << 16) / output_rate */
	uint64_t step64 = ((uint64_t)input_rate << SRC_FRAC_BITS) / output_rate;
	if (step64 > 0xFFFFFFFFULL)
		return 0; /* Overflow */
	return (uint32_t)step64;
}

/**
 * Linearly interpolate between two 16-bit samples.
 * @a:     Sample at position i.
 * @b:     Sample at position i+1.
 * @frac:  Fractional position in 16.16 format (0..SRC_FRAC_UNIT-1).
 *
 * Returns the interpolated 16-bit signed sample.
 */
static inline int16_t lerp16(int16_t a, int16_t b, uint32_t frac)
{
	int32_t diff = (int32_t)b - (int32_t)a;
	int32_t result = (int32_t)a + ((diff * (int32_t)frac) >> SRC_FRAC_BITS);

	/* Clamp to 16-bit range */
	if (result > 32767) result = 32767;
	if (result < -32768) result = -32768;
	return (int16_t)result;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int sound_src_init(struct sound_src_state *src,
                   uint32_t input_rate,
                   uint32_t output_rate,
                   int channels,
                   enum sound_src_quality quality)
{
	if (!src)
		return -EINVAL;

	if (channels < 1 || channels > (int)SOUND_SRC_MAX_CHANNELS)
		return -EINVAL;

	if (quality != SOUND_SRC_NEAREST && quality != SOUND_SRC_LINEAR)
		return -EINVAL;

	uint32_t step = compute_step(input_rate, output_rate);
	if (step == 0)
		return -EINVAL;

	src->input_rate  = input_rate;
	src->output_rate = output_rate;
	src->channels    = channels;
	src->quality     = quality;
	src->phase       = 0;
	src->step        = step;
	src->last_valid  = 0;

	/* Clear the history */
	memset(src->last_frame, 0, sizeof(src->last_frame));

	return 0;
}

int sound_src_process(struct sound_src_state *src,
                      const int16_t *input, uint32_t input_frames,
                      int16_t *output, uint32_t max_output_frames)
{
	if (!src || !input || !output)
		return -EINVAL;

	if (input_frames == 0 || max_output_frames == 0)
		return 0;

	int      ch      = src->channels;
	uint32_t phase   = src->phase;
	uint32_t step    = src->step;
	int      linear  = (src->quality == SOUND_SRC_LINEAR);
	uint32_t oidx    = 0;   /* Output frame index */

	/*
	 * For each output frame we compute the source position:
	 *   pos  = phase >> 16  (integer frame index into input)
	 *   frac = phase & 0xFFFF  (fractional position between frames)
	 *
	 * Linear interpolation blends samples[pos] and samples[pos+1]:
	 *   out = a * (1 - frac) + b * frac   [in fixed point]
	 *
	 * When pos == 0 and we have a valid last_frame from a previous
	 * process() call, we interpolate between last_frame and input[0].
	 */

	while (oidx < max_output_frames) {
		uint32_t pos  = phase >> SRC_FRAC_BITS;
		uint32_t frac = phase & SRC_FRAC_MASK;

		if (pos >= input_frames)
			break;  /* Need more input — save state and return */

		int16_t *out = output + (oidx * ch);

		/*
		 * Determine the two source frames for interpolation.
		 * 'a' is the left frame, 'b' is the right frame.
		 * For nearest-neighbour we just use 'a'.
		 */
		for (int c = 0; c < ch; c++) {
			int16_t a, b;

			if (pos == 0 && src->last_valid) {
				/* Crossing from previous batch: a = last of old,
				 * b = first of new */
				a = src->last_frame[c];
				b = input[c];
			} else {
				a = input[pos * ch + c];
				if (pos + 1 < input_frames)
					b = input[(pos + 1) * ch + c];
				else
					b = a;  /* Last frame — no next */
			}

			if (linear) {
				out[c] = lerp16(a, b, frac);
			} else {
				out[c] = a;  /* Nearest-neighbour */
			}
		}

		oidx++;
		phase += step;
	}

	/* Save the last input frame for the next call's interpolation */
	if (input_frames > 0) {
		const int16_t *last_in = input + ((input_frames - 1) * ch);
		for (int c = 0; c < ch; c++)
			src->last_frame[c] = last_in[c];
		src->last_valid = 1;
	}

	src->phase = phase;
	return (int)oidx;
}

int sound_src_drain(struct sound_src_state *src,
                    int16_t *output, uint32_t max_output_frames)
{
	if (!src || !output)
		return -EINVAL;

	if (max_output_frames == 0)
		return 0;

	/* For downsampling (output_rate < input_rate), the phase
	 * accumulator may have one more output frame pending.
	 * For upsampling, there's nothing to drain. */
	if (src->output_rate >= src->input_rate)
		return 0;

	/* Check if the phase accumulator still has a pending sample */
	uint32_t frac = src->phase & SRC_FRAC_MASK;
	if (frac == 0)
		return 0; /* Exactly aligned — nothing to drain */

	/* One more output frame is available using the last input frame */
	int ch = src->channels;
	int linear = (src->quality == SOUND_SRC_LINEAR);

	for (int c = 0; c < ch; c++) {
		if (linear && src->last_valid) {
			output[c] = lerp16(src->last_frame[c], src->last_frame[c], frac);
		} else {
			output[c] = src->last_frame[c];
		}
	}

	src->phase = 0;
	return 1;
}

void sound_src_reset(struct sound_src_state *src)
{
	if (!src)
		return;

	src->phase      = 0;
	src->last_valid = 0;
	memset(src->last_frame, 0, sizeof(src->last_frame));
}

uint32_t sound_src_estimate_output(const struct sound_src_state *src,
                                    uint32_t input_frames)
{
	if (!src || input_frames == 0)
		return 0;

	/* Upper bound: ceiling(input_frames * output_rate / input_rate) + 1 */
	uint64_t out_frames = (uint64_t)input_frames * src->output_rate;
	out_frames = (out_frames + src->input_rate - 1) / src->input_rate;

	/* Add one for potential drain */
	return (uint32_t)out_frames + 1;
}

int sound_src_convert_rate(const int16_t *input, uint32_t input_frames,
                           int channels,
                           uint32_t input_rate, uint32_t output_rate,
                           int16_t *output, uint32_t max_output_frames)
{
	if (!input || !output)
		return -EINVAL;

	struct sound_src_state src;
	int ret = sound_src_init(&src, input_rate, output_rate,
	                          channels, SOUND_SRC_LINEAR);
	if (ret < 0)
		return ret;

	ret = sound_src_process(&src, input, input_frames,
	                         output, max_output_frames);
	if (ret < 0)
		return ret;

	uint32_t total = (uint32_t)ret;

	if (total < max_output_frames) {
		int drained = sound_src_drain(&src,
		                               output + (total * channels),
		                               max_output_frames - total);
		if (drained > 0)
			total += (uint32_t)drained;
	}

	return (int)total;
}

/* ── Module metadata ────────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("Sample rate converter: phase-accumulator with linear interpolation");
MODULE_AUTHOR("1000 Changes Project");
