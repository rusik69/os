/*
 * sound_mixer_sw.c — Software mixer: multi-stream PCM mixing
 *
 * Implements a software audio mixer that accepts multiple concurrent
 * PCM streams, sums them with per-channel volume control, and produces
 * a single mixed output.
 *
 * Architecture:
 *   - Up to SOUND_MIXER_SW_MAX_CHANNELS (8) virtual input channels.
 *   - Each channel has a per-channel ring buffer and volume.
 *   - sound_mixer_sw_mix() reads frames from every active channel,
 *     applies per-channel volume, sums them with saturation, and
 *     writes the mixed result to the output buffer.
 *   - Channels with no data (empty ring buffer) contribute silence.
 *
 * Locking:
 *   - The top-level mixer lock protects channel allocation/deallocation
 *     and the active_count.
 *   - Each channel's per-channel lock protects its ring buffer cursors.
 *   - The mixing path takes the top-level lock to snapshot the channel
 *     list, then takes each active channel's per-channel lock to read.
 *
 * Task #13 — D142 Software mixer (multi-stream mixing)
 */
#include "sound_mixer_sw.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── Initialisation ────────────────────────────────────────────────── */

int sound_mixer_sw_init(struct sound_mixer_sw *mixer,
			uint8_t pcm_channels,
			uint32_t sample_rate)
{
	if (!mixer)
		return -EINVAL;
	if (pcm_channels < 1 || pcm_channels > SOUND_MIXER_SW_MAX_PCM_CHANNELS)
		return -EINVAL;
	if (sample_rate < 8000 || sample_rate > 192000)
		return -EINVAL;

	memset(mixer, 0, sizeof(*mixer));

	mixer->pcm_channels = pcm_channels;
	mixer->sample_rate  = sample_rate;
	mixer->mix_frames   = SOUND_MIXER_SW_BUFFER_FRAMES;
	mixer->active_count = 0;

	spinlock_init(&mixer->lock);

	/* Allocate the mix accumulator buffer */
	uint32_t mix_buf_bytes = SOUND_MIXER_SW_BUFFER_FRAMES *
				 SOUND_MIXER_SW_MAX_PCM_CHANNELS *
				 sizeof(int16_t);
	mixer->mix_buffer = (int16_t *)kmalloc(mix_buf_bytes);
	if (!mixer->mix_buffer)
		return -ENOMEM;

	memset(mixer->mix_buffer, 0, mix_buf_bytes);

	/* Initialise all channel slots to inactive */
	for (int i = 0; i < SOUND_MIXER_SW_MAX_CHANNELS; i++) {
		struct sound_mixer_sw_channel *ch = &mixer->channels[i];
		ch->active       = 0;
		ch->allocated    = 0;
		ch->volume       = 255;
		ch->mute         = 0;
		ch->buffer       = NULL;
		ch->buf_frames   = 0;
		ch->buf_mask     = 0;
		ch->write_idx    = 0;
		ch->read_idx     = 0;
		ch->pcm_channels = pcm_channels;
		spinlock_init(&ch->lock);
	}

	kprintf("[sound_mixer_sw] Initialised: %d slots, %u Hz, %s\n",
		SOUND_MIXER_SW_MAX_CHANNELS, sample_rate,
		pcm_channels == 2 ? "stereo" : "mono");

	return 0;
}

void sound_mixer_sw_destroy(struct sound_mixer_sw *mixer)
{
	if (!mixer)
		return;

	/* Close all channels and free their ring buffers */
	for (int i = 0; i < SOUND_MIXER_SW_MAX_CHANNELS; i++) {
		struct sound_mixer_sw_channel *ch = &mixer->channels[i];
		if (ch->buffer && ch->allocated) {
			kfree(ch->buffer);
		}
		ch->buffer    = NULL;
		ch->active    = 0;
		ch->allocated = 0;
	}

	/* Free the mix accumulator buffer */
	if (mixer->mix_buffer) {
		kfree(mixer->mix_buffer);
		mixer->mix_buffer = NULL;
	}

	mixer->active_count = 0;
	mixer->mix_frames   = 0;
}

/* ── Channel lifecycle ─────────────────────────────────────────────── */

int sound_mixer_sw_open_channel(struct sound_mixer_sw *mixer, uint8_t volume)
{
	if (!mixer)
		return -EINVAL;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);

	/* Find the first free channel slot */
	int ch_idx = -1;
	for (int i = 0; i < SOUND_MIXER_SW_MAX_CHANNELS; i++) {
		if (!mixer->channels[i].active) {
			ch_idx = i;
			break;
		}
	}

	if (ch_idx < 0) {
		spinlock_irqsave_release(&mixer->lock, irq_flags);
		return -EBUSY;
	}

	struct sound_mixer_sw_channel *ch = &mixer->channels[ch_idx];

	/* Allocate the per-channel ring buffer */
	uint32_t ch_buf_bytes = SOUND_MIXER_SW_CHAN_FRAMES *
				mixer->pcm_channels * sizeof(int16_t);
	ch->buffer = (int16_t *)kmalloc(ch_buf_bytes);
	if (!ch->buffer) {
		spinlock_irqsave_release(&mixer->lock, irq_flags);
		return -ENOMEM;
	}
	memset(ch->buffer, 0, ch_buf_bytes);

	ch->allocated    = 1;
	ch->active       = 1;
	ch->volume       = volume;
	ch->mute         = 0;
	ch->buf_frames   = SOUND_MIXER_SW_CHAN_FRAMES;
	ch->buf_mask     = SOUND_MIXER_SW_CHAN_FRAMES - 1;
	ch->write_idx    = 0;
	ch->read_idx     = 0;
	ch->pcm_channels = mixer->pcm_channels;

	mixer->active_count++;

	spinlock_irqsave_release(&mixer->lock, irq_flags);

	return ch_idx;
}

void sound_mixer_sw_close_channel(struct sound_mixer_sw *mixer, int ch_idx)
{
	if (!mixer || ch_idx < 0 || ch_idx >= SOUND_MIXER_SW_MAX_CHANNELS)
		return;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);

	struct sound_mixer_sw_channel *ch = &mixer->channels[ch_idx];
	if (!ch->active) {
		spinlock_irqsave_release(&mixer->lock, irq_flags);
		return;
	}

	/* Free the channel ring buffer */
	if (ch->buffer && ch->allocated) {
		kfree(ch->buffer);
	}

	ch->buffer    = NULL;
	ch->active    = 0;
	ch->allocated = 0;
	ch->write_idx = 0;
	ch->read_idx  = 0;

	if (mixer->active_count > 0)
		mixer->active_count--;

	spinlock_irqsave_release(&mixer->lock, irq_flags);
}

/* ── Per-channel write ─────────────────────────────────────────────── */

/**
 * Return the number of frames currently available in the channel's
 * ring buffer for the writer (empty space).  Caller must hold ch->lock.
 */
static uint32_t chan_avail_write_locked(struct sound_mixer_sw_channel *ch)
{
	uint32_t filled = ch->write_idx - ch->read_idx;
	if (filled > ch->buf_frames)
		return 0;
	return ch->buf_frames - filled;
}

/**
 * Return the number of frames available for reading (filled data).
 * Caller must hold ch->lock.
 */
static uint32_t chan_avail_read_locked(struct sound_mixer_sw_channel *ch)
{
	uint32_t filled = ch->write_idx - ch->read_idx;
	if (filled > ch->buf_frames)
		return 0;
	return filled;
}

int sound_mixer_sw_write(struct sound_mixer_sw *mixer, int ch_idx,
			 const int16_t *data, uint32_t frames)
{
	if (!mixer || !data)
		return -EINVAL;
	if (ch_idx < 0 || ch_idx >= SOUND_MIXER_SW_MAX_CHANNELS)
		return -EINVAL;

	struct sound_mixer_sw_channel *ch = &mixer->channels[ch_idx];

	if (!ch->active)
		return -ENODEV;
	if (frames == 0)
		return 0;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&ch->lock, &irq_flags);

	uint32_t avail = chan_avail_write_locked(ch);
	if (avail == 0) {
		spinlock_irqsave_release(&ch->lock, irq_flags);
		return 0; /* Buffer full — return 0, caller should retry */
	}

	uint32_t to_write = (frames < avail) ? frames : avail;
	uint32_t written  = 0;
	uint8_t  pcm_ch   = ch->pcm_channels;

	while (written < to_write) {
		uint32_t offset = ch->write_idx & ch->buf_mask;
		uint32_t chunk  = to_write - written;
		uint32_t space  = ch->buf_frames - offset;
		if (chunk > space)
			chunk = space;

		/* Copy interleaved PCM frames into the ring buffer */
		memcpy(ch->buffer + offset * pcm_ch,
		       data + written * pcm_ch,
		       chunk * pcm_ch * sizeof(int16_t));

		ch->write_idx += chunk;
		written += chunk;
	}

	spinlock_irqsave_release(&ch->lock, irq_flags);

	return (int)written;
}

/* ── Mixing engine ─────────────────────────────────────────────────── */

/**
 * Get a pointer to a specific frame in a channel's ring buffer.
 * No locking needed — caller has already acquired ch->lock.
 */
static inline int16_t *chan_frame_ptr(struct sound_mixer_sw_channel *ch,
				      uint32_t idx)
{
	return ch->buffer + ((idx & ch->buf_mask) * ch->pcm_channels);
}

uint32_t sound_mixer_sw_mix(struct sound_mixer_sw *mixer,
			    int16_t *output, uint32_t max_frames)
{
	if (!mixer || !output)
		return 0;
	if (max_frames == 0)
		return 0;

	uint8_t pcm_ch = mixer->pcm_channels;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);

	/* Clamp to the mix buffer capacity */
	uint32_t mix_limit = max_frames;
	if (mix_limit > mixer->mix_frames)
		mix_limit = mixer->mix_frames;

	/*
	 * Determine the minimum number of frames available across all
	 * active, unmuted channels.  We mix only as many frames as the
	 * emptiest channel provides.  If no channel has data, output
	 * silence.
	 */
	uint32_t min_avail = mix_limit;
	int      any_data  = 0;

	for (int i = 0; i < SOUND_MIXER_SW_MAX_CHANNELS; i++) {
		struct sound_mixer_sw_channel *ch = &mixer->channels[i];
		if (!ch->active || ch->mute)
			continue;

		uint64_t ch_flags;
		spinlock_irqsave_acquire(&ch->lock, &ch_flags);
		uint32_t avail = chan_avail_read_locked(ch);
		spinlock_irqsave_release(&ch->lock, ch_flags);

		if (avail > 0) {
			any_data = 1;
			if (avail < min_avail)
				min_avail = avail;
		}
	}

	if (!any_data) {
		/* No data from any active channel — emit silence */
		memset(output, 0, mix_limit * pcm_ch * sizeof(int16_t));
		spinlock_irqsave_release(&mixer->lock, irq_flags);
		return mix_limit;
	}

	if (min_avail > mix_limit)
		min_avail = mix_limit;

	/* Clear the accumulator buffer */
	memset(mixer->mix_buffer, 0, min_avail * pcm_ch * sizeof(int16_t));

	/*
	 * Accumulate: for each active, unmuted channel, read up to
	 * min_avail frames, scale by per-channel volume, and sum into
	 * the accumulator.
	 */
	for (int i = 0; i < SOUND_MIXER_SW_MAX_CHANNELS; i++) {
		struct sound_mixer_sw_channel *ch = &mixer->channels[i];
		if (!ch->active || ch->mute)
			continue;

		uint64_t ch_flags;
		spinlock_irqsave_acquire(&ch->lock, &ch_flags);

		uint32_t avail = chan_avail_read_locked(ch);
		uint32_t take  = (avail < min_avail) ? avail : min_avail;
		if (take == 0) {
			spinlock_irqsave_release(&ch->lock, ch_flags);
			continue;
		}

		uint32_t rd_idx   = ch->read_idx;
		int32_t  vol      = (int32_t)ch->volume;
		uint32_t consumed = 0;

		while (consumed < take) {
			uint32_t offset = rd_idx & ch->buf_mask;
			uint32_t chunk  = take - consumed;
			uint32_t space  = ch->buf_frames - offset;
			if (chunk > space)
				chunk = space;

			for (uint32_t f = 0; f < chunk; f++) {
				for (uint32_t c = 0; c < (uint32_t)pcm_ch; c++) {
					int16_t sample = ch->buffer[(offset + f) * pcm_ch + c];
					int32_t scaled = ((int32_t)sample * vol) / 255;
					mixer->mix_buffer[(consumed + f) * pcm_ch + c] += (int16_t)scaled;
				}
			}

			rd_idx += chunk;
			consumed += chunk;
		}

		/* Advance the channel read cursor */
		ch->read_idx += take;

		spinlock_irqsave_release(&ch->lock, ch_flags);
	}

	/*
	 * Clamp the accumulated result to the 16-bit signed range.
	 * Summing multiple channels can overflow, so we saturate.
	 */
	for (uint32_t f = 0; f < min_avail; f++) {
		for (uint32_t c = 0; c < (uint32_t)pcm_ch; c++) {
			int32_t val = mixer->mix_buffer[f * pcm_ch + c];
			if (val > 32767)
				val = 32767;
			if (val < -32768)
				val = -32768;
			output[f * pcm_ch + c] = (int16_t)val;
		}
	}

	spinlock_irqsave_release(&mixer->lock, irq_flags);

	return min_avail;
}

/* ── Volume / mute controls ────────────────────────────────────────── */

void sound_mixer_sw_set_channel_volume(struct sound_mixer_sw *mixer,
				       int ch_idx, uint8_t volume)
{
	if (!mixer || ch_idx < 0 || ch_idx >= SOUND_MIXER_SW_MAX_CHANNELS)
		return;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);
	mixer->channels[ch_idx].volume = volume;
	spinlock_irqsave_release(&mixer->lock, irq_flags);
}

void sound_mixer_sw_set_channel_mute(struct sound_mixer_sw *mixer,
				     int ch_idx, int mute)
{
	if (!mixer || ch_idx < 0 || ch_idx >= SOUND_MIXER_SW_MAX_CHANNELS)
		return;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);
	mixer->channels[ch_idx].mute = mute ? 1 : 0;
	spinlock_irqsave_release(&mixer->lock, irq_flags);
}

int sound_mixer_sw_get_active_count(struct sound_mixer_sw *mixer)
{
	if (!mixer)
		return 0;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&mixer->lock, &irq_flags);
	int count = mixer->active_count;
	spinlock_irqsave_release(&mixer->lock, irq_flags);

	return count;
}

/* ── Module metadata ────────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("Software audio mixer: multi-stream PCM mixing with per-channel volume");
MODULE_AUTHOR("1000 Changes Project");
