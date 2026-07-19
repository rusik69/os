/*
 * sound_oss.c — OSS /dev/dsp audio interface
 *
 * Implements the Open Sound System /dev/dsp character device for PCM
 * audio playback and capture, using sound_pcm_stream for buffer
 * management.  This sits on top of the AC'97 hardware driver.
 *
 * Supported OSS ioctls:
 *   SNDCTL_DSP_RESET       — Reset the DSP (flush buffers)
 *   SNDCTL_DSP_SPEED       — Set sample rate (8000–48000 Hz)
 *   SNDCTL_DSP_SETFMT      — Set sample format (AFMT_U8, AFMT_S16_LE)
 *   SNDCTL_DSP_CHANNELS    — Set mono (1) / stereo (2)
 *   SNDCTL_DSP_GETBLKSIZE  — Get block size
 *   SNDCTL_DSP_SYNC        — Drain and sync
 *   SNDCTL_DSP_SETFRAGMENT — Set fragment size and count
 *   SNDCTL_DSP_GETOSPACE   — Get output buffer status
 *   SNDCTL_DSP_GETISPACE   — Get input buffer status
 *   SNDCTL_DSP_SETTRIGGER  — Start/stop PCM
 *   SNDCTL_DSP_GETTRIGGER  — Get PCM state
 *   SNDCTL_DSP_POST        — Commit pending writes
 *   SNDCTL_DSP_GETOPTR     — Get playback progress
 *   SNDCTL_DSP_GETIPTR     — Get capture progress
 *   SNDCTL_DSP_GETCAPS     — Get driver capabilities
 *   SOUND_MIXER_*          — Mixer volume/mute/recsrc
 *
 * Task #8 — D142 OSS /dev/dsp implementation (open, read, write, ioctl)
 */

#include "sound_oss.h"
#include "sound_pcm.h"

/* ── Forward declarations ─────────────────────────────────────────── */
void __init sound_oss_init(void);
#include "sound_core.h"
#include "devfs.h"
#include "ac97.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"
#include "uaccess.h"
#include "ioctl.h"

/* ── Driver state ─────────────────────────────────────────────────── */
/* Maximum concurrent open instances (playback + capture) */
#define OSS_MAX_STREAMS  4

/* Default PCM parameters */
#define OSS_DEFAULT_RATE     44100
#define OSS_DEFAULT_CHANNELS 2
#define OSS_DEFAULT_FORMAT   AFMT_S16_LE
#define OSS_DEFAULT_FRAG_SIZE   4096
#define OSS_DEFAULT_FRAG_COUNT  16

/* Playback and capture PCM streams */
static struct sound_pcm_stream *g_playback = NULL;
static struct sound_pcm_stream *g_capture  = NULL;

/* Current audio parameters (set via ioctl, used at stream open time) */
static int g_sample_rate    = OSS_DEFAULT_RATE;
static int g_channels       = OSS_DEFAULT_CHANNELS;
static int g_sample_format  = OSS_DEFAULT_FORMAT;
static int g_sample_width   = 2;   /* bytes per sample (per channel) */
static int g_frag_size      = OSS_DEFAULT_FRAG_SIZE;
static int g_frag_count     = OSS_DEFAULT_FRAG_COUNT;

/* Device open count and trigger state */
static int g_open_count     = 0;
static int g_trigger_state  = 0;  /* bit 0 = capture, bit 1 = playback */
static int g_dsp_initialized = 0;

/* Volume control (0–255, default 192 ≈ 75%) */
static int g_play_volume = 192;
static int g_rec_volume  = 192;

/* DSP volume (separate from mixer — for SNDCTL_DSP_SETPLAYVOL) */
static int g_dsp_play_volume = 255;
static int g_dsp_rec_volume  = 255;

/* DSP profile */
static int g_dsp_profile = DSP_PROFILE_DEFAULT;

/* Error statistics (for SNDCTL_DSP_GETERROR) */
static int g_err_play_underruns    = 0;
static int g_err_rec_overruns      = 0;
static int g_err_play_lost_intr    = 0;
static int g_err_rec_lost_intr     = 0;
static int g_err_play_last_error   = 0;
static int g_err_rec_last_error    = 0;

/* Capture state */
static int g_record_source   = 0;  /* REC_SEL_MIC */
static uint8_t g_record_gain_left  = 10;
static uint8_t g_record_gain_right = 10;
static int g_record_mute    = 0;

/* DMA engine state */
static int g_dma_active     = 0;

/* DMA output buffer (simulates transfer to audio hardware) */
#define DMA_OUT_BUF_SIZE  (64 * 1024)
static uint8_t g_dma_out_buf[DMA_OUT_BUF_SIZE];

/* Protect driver state from concurrent access */
static spinlock_t g_oss_lock;

/* ── Forward declarations ────────────────────────────────────────── */
static int dsp_write(void *priv, const void *data, uint32_t size);
static int dsp_read(void *priv, void *buf,
                    uint32_t max_size, uint32_t *out_size);

/* ── PCM stream lifecycle ─────────────────────────────────────────── */

/**
 * oss_ensure_streams — Allocate PCM streams if not already created.
 * Returns 0 on success, negative errno on failure.
 */
static int oss_ensure_streams(void)
{
	uint64_t irq_flags;
	spinlock_irqsave_acquire(&g_oss_lock, &irq_flags);

	int ret = 0;

	/* Allocate playback stream if not present */
	if (!g_playback) {
		struct sound_pcm_stream *s;

		s = (struct sound_pcm_stream *)kmalloc(
			sizeof(struct sound_pcm_stream));
		if (!s) {
			ret = -ENOMEM;
			goto out;
		}

		ret = sound_pcm_init_stream(s, SOUND_PCM_PLAYBACK,
					    (uint32_t)g_frag_size,
					    (uint32_t)g_frag_count, NULL);
		if (ret < 0) {
			kfree(s);
			goto out;
		}
		g_playback = s;
	}

	/* Allocate capture stream if not present */
	if (!g_capture) {
		struct sound_pcm_stream *s;

		s = (struct sound_pcm_stream *)kmalloc(
			sizeof(struct sound_pcm_stream));
		if (!s) {
			ret = -ENOMEM;
			goto out;
		}

		ret = sound_pcm_init_stream(s, SOUND_PCM_CAPTURE,
					    (uint32_t)g_frag_size,
					    (uint32_t)g_frag_count, NULL);
		if (ret < 0) {
			kfree(s);
			goto out;
		}
		g_capture = s;
	}

out:
	spinlock_irqsave_release(&g_oss_lock, irq_flags);
	return ret;
}

/**
 * oss_destroy_streams — Free both PCM streams.
 */
static void oss_destroy_streams(void)
{
	uint64_t irq_flags;
	spinlock_irqsave_acquire(&g_oss_lock, &irq_flags);

	if (g_playback) {
		sound_pcm_destroy_stream(g_playback);
		kfree(g_playback);
		g_playback = NULL;
	}

	if (g_capture) {
		sound_pcm_destroy_stream(g_capture);
		kfree(g_capture);
		g_capture = NULL;
	}

	spinlock_irqsave_release(&g_oss_lock, irq_flags);
}

/* ── DMA simulation (output to AC97 hardware) ─────────────────────── */

/**
 * dma_transfer_simulate — Drain PCM playback data to AC97 hardware
 * or to the simulated DMA output buffer, with volume scaling.
 */
static void dma_transfer_simulate(void)
{
	if (!g_dma_active || !g_playback)
		return;

	/* Get the next fragment from the PCM stream */
	void *frag_ptr = NULL;
	uint32_t frag_size;

	frag_size = sound_pcm_dma_get_fragment(g_playback, &frag_ptr);
	if (frag_size == 0 || !frag_ptr) {
		g_dma_active = 0;
		return;
	}

	uint8_t tmp[DMA_OUT_BUF_SIZE];
	uint32_t nread = frag_size > DMA_OUT_BUF_SIZE
			 ? DMA_OUT_BUF_SIZE : frag_size;
	memcpy(tmp, frag_ptr, nread);

	/* Apply volume scaling for 16-bit samples */
	if (g_sample_format == AFMT_S16_LE) {
	    int nsamples = (int)(nread / 2);
	    for (int i = 0; i < nsamples; i++) {
	        int16_t s;
	        memcpy(&s, tmp + (size_t)i * 2, sizeof(s));
	        int32_t scaled = s;
	        scaled = (scaled * g_play_volume) / 255;
	        if (scaled > 32767) scaled = 32767;
	        if (scaled < -32768) scaled = -32768;
	        s = (int16_t)scaled;
	        memcpy(tmp + (size_t)i * 2, &s, sizeof(s));
	    }
	} else if (g_sample_format == AFMT_U8) {
		/* Scale for U8 samples */
		for (uint32_t i = 0; i < nread; i++) {
			int32_t s = tmp[i];
			s = (s * g_play_volume) / 255;
			if (s > 255) s = 255;
			if (s < 0) s = 0;
			tmp[i] = (uint8_t)s;
		}
	}

	/* Copy into the DMA output buffer (simulates hardware transfer) */
	memcpy(g_dma_out_buf, tmp, nread);

	/* If hardware is present, also play via AC97 */
	if (ac97_present()) {
		if (g_sample_format == AFMT_U8) {
			int16_t s16_buf[DMA_OUT_BUF_SIZE / 2];
			int nsamples = (int)nread;
			int max_s16 = DMA_OUT_BUF_SIZE / 2;

			for (int i = 0; i < nsamples && i < max_s16; i++)
				s16_buf[i] = (int16_t)((int)tmp[i] * 256
						       - 32768);
			ac97_play_pcm(s16_buf, (uint32_t)(nsamples * 2),
				      (uint32_t)g_sample_rate);
		} else {
			/* Use a properly-aligned copy for unaligned source */
			int16_t aligned_tmp[DMA_OUT_BUF_SIZE / 2];
			memcpy(aligned_tmp, tmp, nread);
			ac97_play_pcm(aligned_tmp, nread,
				      (uint32_t)g_sample_rate);
		}
	}

	/* Consume the fragment from the PCM stream */
	sound_pcm_dma_consume(g_playback);

	/* If the buffer is empty now, stop DMA simulation */
	if (sound_pcm_get_avail_read(g_playback) == 0)
		g_dma_active = 0;
}

/* ── devfs callbacks ──────────────────────────────────────────────── */

static int dsp_write(void *priv, const void *data, uint32_t size)
{
	(void)priv;
	if (!data || size == 0)
		return 0;

	/* Ensure PCM streams are allocated */
	if (!g_playback) {
		int ret = oss_ensure_streams();
		if (ret < 0)
			return 0;
	}

	/* Write data into the PCM playback stream */
	int written = sound_pcm_write(g_playback, data, size);
	if (written < 0)
		written = 0;

	/* Start DMA transfer simulation if data was written */
	if (written > 0) {
		g_dma_active = 1;
		dma_transfer_simulate();
	}

	/* OSS semantics: return the requested size as "consumed" */
	return (int)size;
}

static int dsp_read(void *priv, void *buf,
		    uint32_t max_size, uint32_t *out_size)
{
	(void)priv;
	if (!buf || max_size == 0) {
		if (out_size) *out_size = 0;
		return 0;
	}

	/* If AC97 hardware is not present, return silence */
	if (!ac97_present()) {
		if (out_size) *out_size = 0;
		return 0;
	}

	/* Apply recording gain/mute settings */
	ac97_set_record_source((uint16_t)g_record_source);
	ac97_set_record_gain(g_record_gain_left, g_record_gain_right,
			     g_record_mute);

	/* Capture audio samples from the selected input source */
	uint32_t bytes_to_capture = max_size;

	/* Clamp to a reasonable single-shot capture (128 KB max) */
	if (bytes_to_capture > 128 * 1024)
		bytes_to_capture = 128 * 1024;

	/* Perform the hardware capture */
	int captured = ac97_capture_read((int16_t *)buf, bytes_to_capture,
					 (uint32_t)g_sample_rate);

	if (captured < 0) {
		/* Capture failed — return silence */
		uint32_t silence_len = max_size > 4096 ? 4096 : max_size;
		memset(buf, 0, silence_len);
		captured = (int)silence_len;
	}

	/* Downmix stereo capture to mono if requested */
	if (g_channels == 1 && captured > 0 &&
	    g_sample_format == AFMT_S16_LE) {
		int16_t *samples = (int16_t *)buf;
		int num_samples = captured / 2;
		int stereo_pairs = num_samples / 2;

		for (int i = 0; i < stereo_pairs; i++) {
			int32_t sum = (int32_t)samples[i * 2] +
				      (int32_t)samples[i * 2 + 1];
			samples[i] = (int16_t)(sum / 2);
		}
		captured = stereo_pairs * 2;
	}

	/* Convert S16_LE to U8 if requested */
	if (g_sample_format == AFMT_U8 && captured > 0) {
		int16_t *s16 = (int16_t *)buf;
		uint8_t *u8  = (uint8_t *)buf;
		int num_samples = captured / 2;

		for (int i = 0; i < num_samples; i++) {
			int32_t val = ((int32_t)s16[i] + 32768) >> 8;
			if (val < 0) val = 0;
			if (val > 255) val = 255;
			u8[i] = (uint8_t)val;
		}
		captured = num_samples;
	}

	if (out_size) *out_size = (uint32_t)captured;
	return 0;
}

/* ── IOCTL argument helpers (safe userspace access) ───────────────── */

/**
 * oss_read_arg32 — Read a uint32_t argument from the ioctl userspace ptr.
 * Returns 0 on success or -EFAULT on error.
 */
static int oss_read_arg32(uint64_t arg, uint32_t *out)
{
	return copy_from_user(out, arg, sizeof(uint32_t)) < 0
	       ? -EFAULT : 0;
}

/**
 * oss_write_arg32 — Write a uint32_t to the ioctl userspace ptr.
 * Returns 0 on success or -EFAULT on error.
 */
static int oss_write_arg32(uint64_t arg, uint32_t val)
{
	return copy_to_user(arg, &val, sizeof(uint32_t)) < 0
	       ? -EFAULT : 0;
}

/**
 * oss_readwrite_arg — Read or write a uint32_t argument depending on
 * the ioctl direction bits.
 *
 * For write-only (_IOC_WRITE): read *arg from user, return the value.
 * For read-only (_IOC_READ):  write val to user, return 0.
 * For read-write (_IOC_READ|_IOC_WRITE): read first, write back.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int oss_readwrite_arg(uint64_t arg, uint32_t cmd,
			     uint32_t *in_val, uint32_t *out_val)
{
	uint32_t dir = _IOC_DIR(cmd);

	if (dir & _IOC_WRITE) {
		/* Data flows from user to kernel — read it */
		uint32_t val;
		int ret = oss_read_arg32(arg, &val);
		if (ret < 0)
			return ret;
		*in_val = val;
	}

	if (dir & _IOC_READ) {
		/* Data flows from kernel to user — write it */
		int ret = oss_write_arg32(arg, *out_val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/**
 * oss_copyout_buf — Copy a fixed-size buffer to userspace.
 * Returns 0 on success, -EFAULT on error.
 */
static int oss_copyout_buf(uint64_t arg, const void *buf, uint32_t size)
{
	return copy_to_user(arg, buf, (uint32_t)size) < 0 ? -EFAULT : 0;
}

/* ── Public API: OSS ioctl handler ────────────────────────────────── */

int sound_oss_ioctl(int cmd, uint64_t arg)
{
	int ret = 0;

	switch (cmd) {

	/* ── DSP reset ─────────────────────────────────────────── */
	case SNDCTL_DSP_RESET:
		if (g_playback)
			sound_pcm_reset(g_playback);
		if (g_capture)
			sound_pcm_reset(g_capture);
		g_frag_size     = OSS_DEFAULT_FRAG_SIZE;
		g_frag_count    = OSS_DEFAULT_FRAG_COUNT;
		g_sample_rate   = OSS_DEFAULT_RATE;
		g_channels      = OSS_DEFAULT_CHANNELS;
		g_sample_format = OSS_DEFAULT_FORMAT;
		g_sample_width  = 2;
		g_dma_active    = 0;
		return 0;

	/* ── DSP sync (drain) ──────────────────────────────────── */
	case SNDCTL_DSP_SYNC:
		if (g_playback)
			sound_pcm_drain(g_playback);
		g_dma_active = 0;
		return 0;

	/* ── DSP speed (sample rate) ───────────────────────────── */
	case SNDCTL_DSP_SPEED: {
		uint32_t rate;
		ret = oss_read_arg32(arg, &rate);
		if (ret < 0)
			return ret;

		/* Validate/clamp sample rate against codec capabilities */
		g_sample_rate = (int)ac97_sanitise_sample_rate(rate);

		/* Reconfigure streams if active */
		if (g_playback) {
			sound_pcm_reset(g_playback);
			sound_pcm_init_stream(g_playback,
					      SOUND_PCM_PLAYBACK,
					      (uint32_t)g_frag_size,
					      (uint32_t)g_frag_count, NULL);
		}
		if (g_capture) {
			sound_pcm_reset(g_capture);
			sound_pcm_init_stream(g_capture,
					      SOUND_PCM_CAPTURE,
					      (uint32_t)g_frag_size,
					      (uint32_t)g_frag_count, NULL);
		}

		/* Write back the actual rate */
		return oss_write_arg32(arg, (uint32_t)g_sample_rate);
	}

	/* ── DSP format ────────────────────────────────────────── */
	case SNDCTL_DSP_SETFMT: {
		uint32_t fmt;
		ret = oss_read_arg32(arg, &fmt);
		if (ret < 0)
			return ret;

		switch ((int)fmt) {
		case AFMT_U8:
			g_sample_format = AFMT_U8;
			g_sample_width  = 1;
			break;
		case AFMT_S16_LE:
			g_sample_format = AFMT_S16_LE;
			g_sample_width  = 2;
			break;
		case AFMT_S16_BE:
			g_sample_format = AFMT_S16_BE;
			g_sample_width  = 2;
			break;
		default:
			/* Fall back to AFMT_S16_LE */
			g_sample_format = AFMT_S16_LE;
			g_sample_width  = 2;
			break;
		}

		/* Write back the actual format */
		return oss_write_arg32(arg, (uint32_t)g_sample_format);
	}

	/* ── DSP channels ──────────────────────────────────────── */
	case SNDCTL_DSP_CHANNELS: {
		uint32_t ch;
		ret = oss_read_arg32(arg, &ch);
		if (ret < 0)
			return ret;

		if ((int)ch < 1) ch = 1;
		if ((int)ch > 2) ch = 2;
		g_channels = (int)ch;

		/* Write back the actual channel count */
		return oss_write_arg32(arg, ch);
	}

	/* ── DSP get block size ────────────────────────────────── */
	case SNDCTL_DSP_GETBLKSIZE:
		return oss_write_arg32(arg, (uint32_t)g_frag_size);

	/* ── DSP set fragment size/count ───────────────────────── */
	case SNDCTL_DSP_SETFRAGMENT: {
		uint32_t fragspec;
		int old_frag_size = g_frag_size;
		int old_frag_count = g_frag_count;

		ret = oss_read_arg32(arg, &fragspec);
		if (ret < 0)
			return ret;

		int frag_shift = (int)(fragspec & 0xFFFF);
		int num_frags  = (int)((fragspec >> 16) & 0xFFFF);

		if (frag_shift < 4)  frag_shift = 4;   /* min 16 bytes */
		if (frag_shift > 15) frag_shift = 15;  /* max 32K */
		if (num_frags < 2)   num_frags = 2;
		if (num_frags > 64)  num_frags = 64;

		g_frag_size  = 1U << frag_shift;
		g_frag_count = num_frags;

		/* Re-initialise streams with new fragment parameters
		 * if they already exist */
		if (g_playback && g_frag_size != old_frag_size) {
			sound_pcm_reset(g_playback);
			sound_pcm_init_stream(g_playback,
					      SOUND_PCM_PLAYBACK,
					      (uint32_t)g_frag_size,
					      (uint32_t)g_frag_count,
					      NULL);
		}
		if (g_capture && g_frag_size != old_frag_size) {
			sound_pcm_reset(g_capture);
			sound_pcm_init_stream(g_capture,
					      SOUND_PCM_CAPTURE,
					      (uint32_t)g_frag_size,
					      (uint32_t)g_frag_count,
					      NULL);
		}

		/* OSS semantics: write back the actual fragment spec */
		uint32_t actual_fragspec =
			((uint32_t)(31 - __builtin_clz(
				(uint32_t)g_frag_size)) & 0xFFFF) |
			(((uint32_t)g_frag_count & 0xFFFF) << 16);
		return oss_write_arg32(arg, actual_fragspec);
	}

	/* ── DSP get output space ──────────────────────────────── */
	case SNDCTL_DSP_GETOSPACE: {
		struct audio_buf_info info;
		int avail_bytes = 0;

		if (g_playback)
			avail_bytes = (int)sound_pcm_get_avail_write(
					g_playback);
		else
			avail_bytes = g_frag_size * g_frag_count;

		info.fragments  = g_frag_size > 0
				  ? avail_bytes / g_frag_size : 0;
		info.fragstotal = g_frag_count;
		info.fragsize   = g_frag_size;
		info.bytes      = avail_bytes;

		return oss_copyout_buf(arg, &info, sizeof(info));
	}

	/* ── DSP get input space ───────────────────────────────── */
	case SNDCTL_DSP_GETISPACE: {
		struct audio_buf_info info;
		int avail_bytes = 0;

		if (g_capture)
			avail_bytes = (int)sound_pcm_get_avail_write(
					g_capture);
		else
			avail_bytes = g_frag_size * g_frag_count;

		info.fragments  = g_frag_size > 0
				  ? avail_bytes / g_frag_size : 0;
		info.fragstotal = g_frag_count;
		info.fragsize   = g_frag_size;
		info.bytes      = avail_bytes;

		return oss_copyout_buf(arg, &info, sizeof(info));
	}

	/* ── DSP trigger (start/stop PCM) ──────────────────────── */
	case SNDCTL_DSP_SETTRIGGER: {
		uint32_t trigger;
		ret = oss_read_arg32(arg, &trigger);
		if (ret < 0)
			return ret;

		g_trigger_state = (int)trigger;

		if (trigger & PCM_ENABLE_OUTPUT) {
			g_dma_active = 1;
			if (g_playback)
				dma_transfer_simulate();
		}
		return 0;
	}

	/* ── DSP get trigger state ─────────────────────────────── */
	case SNDCTL_DSP_GETTRIGGER:
		return oss_write_arg32(arg, (uint32_t)g_trigger_state);

	/* ── DSP get capabilities ──────────────────────────────── */
	case SNDCTL_DSP_GETCAPS: {
		uint32_t caps = DSP_CAP_TRIGGER | DSP_CAP_DUPLEX;
		return oss_write_arg32(arg, caps);
	}

	/* ── DSP post (commit writes) ──────────────────────────── */
	case SNDCTL_DSP_POST:
		g_dma_active = 1;
		if (g_playback)
			dma_transfer_simulate();
		return 0;

	/* ── DSP get output pointer ────────────────────────────── */
	case SNDCTL_DSP_GETOPTR: {
		struct count_info info;
		uint32_t total_bytes = 0;

		if (g_playback)
			total_bytes = g_playback->app_ptr;

		info.bytes  = (int)total_bytes;
		info.blocks = g_frag_size > 0
			      ? (int)(total_bytes /
				      (uint32_t)g_frag_size) : 0;
		info.ptr    = g_playback
			      ? (int)(g_playback->hw_ptr &
				      (g_playback->buf_mask))
			      : 0;

		return oss_copyout_buf(arg, &info, sizeof(info));
	}

	/* ── DSP get input pointer ─────────────────────────────── */
	case SNDCTL_DSP_GETIPTR: {
		struct count_info info;
		uint32_t total_bytes = 0;

		if (g_capture)
			total_bytes = g_capture->hw_ptr;

		info.bytes  = (int)total_bytes;
		info.blocks = g_frag_size > 0
			      ? (int)(total_bytes /
				      (uint32_t)g_frag_size) : 0;
		info.ptr    = g_capture
			      ? (int)(g_capture->app_ptr &
				      (g_capture->buf_mask))
			      : 0;

		return oss_copyout_buf(arg, &info, sizeof(info));
	}

	/* ── Record source ─────────────────────────────────────── */
	case SNDCTL_DSP_SETRECORD_SOURCE: {
		uint32_t src;
		ret = oss_read_arg32(arg, &src);
		if (ret < 0)
			return ret;

		g_record_source = (int)src;
		if (ac97_present())
			ac97_set_record_source((uint16_t)g_record_source);
		return 0;
	}

	case SNDCTL_DSP_GETRECORD_SOURCE:
		return oss_write_arg32(arg, (uint32_t)g_record_source);

	/* ── Record gain ───────────────────────────────────────── */
	case SNDCTL_DSP_SETRECORD_GAIN: {
		uint32_t gain;
		ret = oss_read_arg32(arg, &gain);
		if (ret < 0)
			return ret;

		g_record_gain_left  = (uint8_t)(gain & 0xFF);
		g_record_gain_right = (uint8_t)((gain >> 8) & 0xFF);
		g_record_mute       = (gain & 0x8000) ? 1 : 0;
		if (ac97_present())
			ac97_set_record_gain(g_record_gain_left,
					     g_record_gain_right,
					     g_record_mute);
		return 0;
	}

	case SNDCTL_DSP_GETRECORD_GAIN: {
		uint32_t val;

		val = (uint32_t)g_record_gain_left |
		      ((uint32_t)g_record_gain_right << 8);
		if (g_record_mute)
			val |= 0x8000;
		return oss_write_arg32(arg, val);
	}

	/* ── DSP get formats (SNDCTL_DSP_GETFMTS) ─────────────── */
	case SNDCTL_DSP_GETFMTS: {
		uint32_t fmts = AFMT_BIT_U8 |
				AFMT_BIT_S16_LE |
				AFMT_BIT_S16_BE;
		return oss_write_arg32(arg, fmts);
	}

	/* ── DSP get output delay (SNDCTL_DSP_GETODELAY) ──────── */
	case SNDCTL_DSP_GETODELAY: {
		uint32_t delay = 0;
		if (g_playback)
			delay = sound_pcm_get_avail_read(g_playback);
		return oss_write_arg32(arg, delay);
	}

	/* ── DSP get channels (SNDCTL_DSP_GETCHANNELS) ────────── */
	case SNDCTL_DSP_GETCHANNELS: {
		uint32_t ch = (uint32_t)g_channels;
		if (ch < 1) ch = 1;
		if (ch > 2) ch = 2;
		return oss_write_arg32(arg, ch);
	}

	/* ── DSP subdivide (SNDCTL_DSP_SUBDIVIDE) ─────────────── */
	case SNDCTL_DSP_SUBDIVIDE: {
		uint32_t div;
		ret = oss_read_arg32(arg, &div);
		if (ret < 0)
			return ret;
		/* Accept 1 (no divide) — could support 2,4 for
		 * fragment subdivision, but we always return 1. */
		return oss_write_arg32(arg, 1);
	}

	/* ── DSP playback volume (SNDCTL_DSP_SETPLAYVOL) ──────── */
	case SNDCTL_DSP_SETPLAYVOL: {
		uint32_t vol;
		ret = oss_read_arg32(arg, &vol);
		if (ret < 0)
			return ret;
		g_dsp_play_volume = (int)(vol & 0xFF);
		if (g_dsp_play_volume > 100)
			g_dsp_play_volume = 100;
		return 0;
	}

	/* ── DSP playback volume (SNDCTL_DSP_GETPLAYVOL) ──────── */
	case SNDCTL_DSP_GETPLAYVOL: {
		uint32_t vol = (uint32_t)g_dsp_play_volume |
			       ((uint32_t)g_dsp_play_volume << 8);
		return oss_write_arg32(arg, vol);
	}

	/* ── DSP recording volume (SNDCTL_DSP_SETRECVOL) ──────── */
	case SNDCTL_DSP_SETRECVOL: {
		uint32_t vol;
		ret = oss_read_arg32(arg, &vol);
		if (ret < 0)
			return ret;
		g_dsp_rec_volume = (int)(vol & 0xFF);
		if (g_dsp_rec_volume > 100)
			g_dsp_rec_volume = 100;
		return 0;
	}

	/* ── DSP recording volume (SNDCTL_DSP_GETRECVOL) ──────── */
	case SNDCTL_DSP_GETRECVOL: {
		uint32_t vol = (uint32_t)g_dsp_rec_volume |
			       ((uint32_t)g_dsp_rec_volume << 8);
		return oss_write_arg32(arg, vol);
	}

	/* ── DSP profile (SNDCTL_DSP_PROFILE) ─────────────────── */
	case SNDCTL_DSP_PROFILE: {
		uint32_t profile;
		ret = oss_read_arg32(arg, &profile);
		if (ret < 0)
			return ret;
		switch ((int)profile) {
		case DSP_PROFILE_DEFAULT:
		case DSP_PROFILE_LOW_LATENCY:
		case DSP_PROFILE_HIGH_Q:
			g_dsp_profile = (int)profile;
			break;
		default:
			g_dsp_profile = DSP_PROFILE_DEFAULT;
			break;
		}
		return oss_write_arg32(arg,
				       (uint32_t)g_dsp_profile);
	}

	/* ── DSP get error (SNDCTL_DSP_GETERROR) ──────────────── */
	case SNDCTL_DSP_GETERROR: {
		struct audio_errinfo err;
		err.play_underruns       = g_err_play_underruns;
		err.rec_overruns         = g_err_rec_overruns;
		err.play_ptradjust       = 0UL;
		err.rec_ptradjust        = 0UL;
		err.play_error_count     = g_err_play_underruns;
		err.rec_error_count      = g_err_rec_overruns;
		err.play_lost_interrupts = g_err_play_lost_intr;
		err.rec_lost_interrupts  = g_err_rec_lost_intr;
		err.play_last_error      = g_err_play_last_error;
		err.rec_last_error       = g_err_rec_last_error;
		return oss_copyout_buf(arg, &err, sizeof(err));
	}

	/* ── OSS Mixer ioctls ──────────────────────────────────── */

	case SOUND_MIXER_READ_VOLUME: {
		uint16_t mv = sound_mixer_read(SOUND_MIXER_MASTER);
		return oss_write_arg32(arg, (uint32_t)mv);
	}

	case SOUND_MIXER_WRITE_VOLUME: {
		uint32_t val;
		ret = oss_read_arg32(arg, &val);
		if (ret < 0)
			return ret;
		ret = sound_mixer_write(SOUND_MIXER_MASTER,
					(uint16_t)val);
		if (ret < 0)
			return ret;
		/* Write back the actual value after clamping
		 * (OSS mixer ioctls are read-write — dir=3). */
		uint16_t actual = sound_mixer_read(SOUND_MIXER_MASTER);
		return oss_write_arg32(arg, (uint32_t)actual);
	}

	case SOUND_MIXER_READ_MUTE: {
		uint16_t mv = sound_mixer_read(SOUND_MIXER_MASTER);
		return oss_write_arg32(arg, (mv & 0x8000) ? 1U : 0U);
	}

	case SOUND_MIXER_WRITE_MUTE: {
		uint32_t val;
		ret = oss_read_arg32(arg, &val);
		if (ret < 0)
			return ret;
		ret = sound_mixer_set_mute(SOUND_MIXER_MASTER,
					(int)val ? 1 : 0);
		if (ret < 0)
			return ret;
		/* Write back the actual mute state
		 * (OSS mixer ioctls are read-write — dir=3). */
		uint16_t mv = sound_mixer_read(SOUND_MIXER_MASTER);
		return oss_write_arg32(arg,
				(mv & 0x8000) ? 1U : 0U);
	}

	case SOUND_MIXER_READ_DEVMASK:
		/* Report available: master, pcm, mic, linein, cd, speaker */
		return oss_write_arg32(arg, 0x3F);

	case SOUND_MIXER_READ_RECMASK:
		/* Only mic supports recording */
		return oss_write_arg32(arg, (1u << SOUND_MIXER_MIC));

	case SOUND_MIXER_READ_RECSRC: {
		uint32_t mask = 0;
		for (int i = 0; i < SOUND_MIXER_COUNT; i++) {
			if (g_sound_mixer[i].recsel)
				mask |= (1u << i);
		}
		return oss_write_arg32(arg, mask);
	}

	case SOUND_MIXER_WRITE_RECSRC: {
		uint32_t mask;
		ret = oss_read_arg32(arg, &mask);
		if (ret < 0)
			return ret;
		for (int i = 0; i < SOUND_MIXER_COUNT; i++) {
			ret = sound_mixer_set_recsel(
				(enum sound_mixer_channel)i,
				(mask & (1u << i)) ? 1 : 0);
			if (ret < 0)
				return ret;
		}
		/* Write back the actual rec source mask
		 * (OSS mixer ioctls are read-write — dir=3). */
		uint32_t actual_mask = 0;
		for (int i = 0; i < SOUND_MIXER_COUNT; i++) {
			if (g_sound_mixer[i].recsel)
				actual_mask |= (1u << i);
		}
		return oss_write_arg32(arg, actual_mask);
	}

	case SOUND_MIXER_READ_STEREO:
		/* All our channels are stereo */
		return oss_write_arg32(arg, 1);

	case SOUND_MIXER_READ_CAPS:
		/* No special caps */
		return oss_write_arg32(arg, 0);

	default:
		return -ENOTTY;
	}
}

/* ── Public API: devfs open / release stubs ──────────────────────── */

static int sound_oss_open(int dev, void *file)
{
	(void)dev;
	(void)file;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&g_oss_lock, &irq_flags);

	if (g_open_count >= OSS_MAX_STREAMS) {
		spinlock_irqsave_release(&g_oss_lock, irq_flags);
		return -EBUSY;
	}

	/* Ensure PCM streams are ready */
	int ret;
	spinlock_irqsave_release(&g_oss_lock, irq_flags);
	ret = oss_ensure_streams();
	spinlock_irqsave_acquire(&g_oss_lock, &irq_flags);
	if (ret < 0) {
		spinlock_irqsave_release(&g_oss_lock, irq_flags);
		return ret;
	}

	g_open_count++;

	spinlock_irqsave_release(&g_oss_lock, irq_flags);
	return 0;
}

static int sound_oss_release(int dev, void *file)
{
	(void)dev;
	(void)file;

	uint64_t irq_flags;
	spinlock_irqsave_acquire(&g_oss_lock, &irq_flags);

	if (g_open_count > 0)
		g_open_count--;

	if (g_open_count == 0) {
		/* Reset streams on last close */
		if (g_playback)
			sound_pcm_reset(g_playback);
		if (g_capture)
			sound_pcm_reset(g_capture);
		g_dma_active = 0;
		g_trigger_state = 0;
	}

	spinlock_irqsave_release(&g_oss_lock, irq_flags);
	return 0;
}

static int sound_oss_read(int dev, void *buf, size_t count)
{
	(void)dev;
	(void)buf;
	(void)count;

	/* Legacy stub — actual reads use devfs dsp_read callback.
	 * Kept for future VFS-level character device support. */
	return 0;
}

static int sound_oss_write(int dev, const void *buf, size_t count)
{
	(void)dev;
	(void)buf;
	(void)count;

	/* Legacy stub — actual writes use devfs dsp_write callback.
	 * Kept for future VFS-level character device support. */
	return 0;
}

/* ── Initialisation ──────────────────────────────────────────────── */

void __init sound_oss_init(void)
{
	if (g_dsp_initialized)
		return;

	/* Only register if AC97 audio hardware is present */
	if (!ac97_present())
		return;

	spinlock_init(&g_oss_lock);

	g_sample_rate   = OSS_DEFAULT_RATE;
	g_channels      = OSS_DEFAULT_CHANNELS;
	g_sample_format = OSS_DEFAULT_FORMAT;
	g_sample_width  = 2;
	g_frag_size     = OSS_DEFAULT_FRAG_SIZE;
	g_frag_count    = OSS_DEFAULT_FRAG_COUNT;

	/* Register /dev/dsp — PCM audio playback and capture */
	int ret = devfs_register_device("dsp", NULL, dsp_read, dsp_write);
	if (ret == 0) {
		kprintf("[OK] OSS: /dev/dsp registered (%d Hz %d ch %d-bit)\n",
			g_sample_rate, g_channels, g_sample_width * 8);
		g_dsp_initialized = 1;
	} else {
		kprintf("[OSS] WARN: failed to register /dev/dsp\n");
	}

	/* Register /dev/mixer — volume control (ioctl-only device).
	 * The mixer ioctls (SOUND_MIXER_READ_VOLUME, etc.) are dispatched
	 * via sys_ioctl.c to sound_oss_ioctl() and work on any fd, but
	 * userspace expects to open /dev/mixer explicitly per OSS spec. */
	ret = devfs_register_device("mixer", NULL, NULL, NULL);
	if (ret == 0) {
		kprintf("[OK] OSS: /dev/mixer registered\n");
	} else {
		kprintf("[OSS] WARN: failed to register /dev/mixer\n");
	}
}

/* ── Module metadata ─────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("OSS /dev/dsp audio interface with PCM stream support");
MODULE_AUTHOR("1000 Changes Project");
module_init(sound_oss_init);
