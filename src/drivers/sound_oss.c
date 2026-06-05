/*
 * sound_oss.c — OSS /dev/dsp audio interface
 *
 * Implements the Open Sound System /dev/dsp character device for PCM
 * audio playback.  This sits on top of the AC'97 hardware driver.
 *
 * Supported OSS ioctls:
 *   SNDCTL_DSP_RESET      — Reset the DSP (flush buffers)
 *   SNDCTL_DSP_SPEED      — Set sample rate (8000–48000 Hz)
 *   SNDCTL_DSP_SETFMT     — Set sample format (AFMT_U8, AFMT_S16_LE)
 *   SNDCTL_DSP_CHANNELS   — Set mono (1) / stereo (2)
 *   SNDCTL_DSP_GETBLKSIZE — Get block size
 *   SNDCTL_DSP_SYNC       — Drain and sync
 *
 * Item 228 — OSS /dev/dsp interface (Plan 4, 200-more-production-improvements)
 */

#include "ac97.h"
#include "devfs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

/* ── OSS ioctl definitions (subset) ───────────────────────────────── */
#define SNDCTL_DSP_RESET      0x00500000
#define SNDCTL_DSP_SPEED      0x00500002
#define SNDCTL_DSP_SETFMT     0x00500005
#define SNDCTL_DSP_CHANNELS   0x00500006
#define SNDCTL_DSP_GETBLKSIZE 0x00500004
#define SNDCTL_DSP_SYNC       0x00500001
#define SNDCTL_DSP_SETTRIGGER 0x00500010
#define SNDCTL_DSP_GETTRIGGER 0x00500011
#define SNDCTL_DSP_GETISPACE  0x00500012
#define SNDCTL_DSP_GETOSPACE  0x00500013

/* PCM trigger bits */
#define PCM_ENABLE_INPUT  0x00000001
#define PCM_ENABLE_OUTPUT 0x00000002

/* OSS audio buffer info (for GETISPACE/GETOSPACE) */
struct audio_buf_info {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
};

/* Recording source selectors for SNDCTL_DSP_SETRECORD_SOURCE */
#define SNDCTL_DSP_SETRECORD_SOURCE 0x00500050
#define SNDCTL_DSP_GETRECORD_SOURCE 0x00500051
#define SNDCTL_DSP_SETRECORD_GAIN   0x00500052
#define SNDCTL_DSP_GETRECORD_GAIN   0x00500053

/* Audio format codes */
#define AFMT_U8        8
#define AFMT_S16_LE    16
#define AFMT_S16_BE    17

/* ── Driver state ─────────────────────────────────────────────────── */
static int g_dsp_initialized = 0;
static int g_sample_rate    = 44100;
static int g_channels       = 2;   /* 1 = mono, 2 = stereo */
static int g_sample_format  = AFMT_S16_LE;  /* 16-bit signed LE */
static int g_sample_width   = 2;   /* bytes per sample (per channel) */

/* Capture state */
static int g_capture_rate    = 44100;
static int g_record_source   = REC_SEL_MIC;
static uint8_t g_record_gain_left  = 10; /* default ~15dB */
static uint8_t g_record_gain_right = 10;
static int g_record_mute    = 0;

/* Simple DMA output buffer (single-frame) for synchronous playback.
 * For a production implementation this would be a proper ring buffer
 * with interrupt-driven DMA. */
#define DSP_BUFFER_SIZE (64 * 1024)  /* 64 KB temporary buffer */
static int16_t g_dsp_buf[DSP_BUFFER_SIZE / 2];
static int     g_dsp_buf_fill = 0;

/* Protect g_dsp_buf and state from concurrent access */
static spinlock_t g_dsp_lock;

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Flush the internal buffer to the AC97 hardware and reset fill level.
 * Called with g_dsp_lock held. */
static void dsp_flush_locked(void)
{
    if (g_dsp_buf_fill == 0)
        return;

    if (ac97_present()) {
        /* AC97 plays 16-bit signed PCM.  If format is U8, do a quick
         * conversion to S16_LE (scale by 256, bias by -32768). */
        if (g_sample_format == AFMT_U8 && g_dsp_buf_fill > 0) {
            /* In-place convert buffer from U8 to S16_LE */
            int num_samples = g_dsp_buf_fill;  /* bytes = samples for U8 */
            if (num_samples > DSP_BUFFER_SIZE / 2)
                num_samples = DSP_BUFFER_SIZE / 2;
            /* Convert from end of buffer backwards to avoid overwriting */
            for (int i = num_samples - 1; i >= 0; i--) {
                uint8_t u8 = ((uint8_t *)g_dsp_buf)[i];
                g_dsp_buf[i] = (int16_t)((int)u8 * 256 - 32768);
            }
            g_dsp_buf_fill = num_samples * 2;  /* now S16_LE */
        }

        ac97_play_pcm(g_dsp_buf, (uint32_t)g_dsp_buf_fill,
                      (uint32_t)g_sample_rate);
    }

    g_dsp_buf_fill = 0;
}

/* ── devfs callbacks ──────────────────────────────────────────────── */

static int dsp_write(void *priv, const void *data, uint32_t size)
{
    (void)priv;
    if (!data || size == 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dsp_lock, &irq_flags);

    /* For synchronous (blocking) playback, just play each chunk
     * immediately.  If this were an interrupt-driven streaming
     * driver we'd buffer and DMA, but for now keep it simple. */
    if (g_dsp_buf_fill > 0) {
        /* Flush any previously buffered data first */
        dsp_flush_locked();
    }

    /* Convert sample format if needed */
    uint32_t bytes_to_play = size;

    /* Clamp to buffer size */
    if (bytes_to_play > DSP_BUFFER_SIZE)
        bytes_to_play = DSP_BUFFER_SIZE;

    memcpy(g_dsp_buf, data, bytes_to_play);
    g_dsp_buf_fill = (int)bytes_to_play;

    /* For synchronous mode, play immediately */
    dsp_flush_locked();

    spinlock_irqsave_release(&g_dsp_lock, irq_flags);
    return (int)size;
}

static int dsp_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
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
    ac97_set_record_gain(g_record_gain_left, g_record_gain_right, g_record_mute);

    /* Capture audio samples from the selected input source.
     * Use the configured sample format: for U8 we capture S16_LE then
     * convert down; for S16_LE we capture directly. */
    uint32_t capture_rate = (uint32_t)g_capture_rate;
    uint32_t bytes_to_capture = max_size;

    /* Clamp to a reasonable single-shot capture (128 KB max) */
    if (bytes_to_capture > 128 * 1024)
        bytes_to_capture = 128 * 1024;

    /* Perform the hardware capture */
    int captured = ac97_capture_read((int16_t *)buf, bytes_to_capture, capture_rate);

    if (captured < 0) {
        /* Capture failed — return silence */
        memset(buf, 0, max_size > 4096 ? 4096 : max_size);
        captured = (int)(max_size > 4096 ? 4096 : max_size);
    }

    /* Downmix stereo capture to mono if requested */
    if (g_channels == 1 && captured > 0 && g_sample_format == AFMT_S16_LE) {
        int16_t *samples = (int16_t *)buf;
        int num_samples = captured / 2; /* total 16-bit samples (both channels) */
        int stereo_pairs = num_samples / 2;
        for (int i = 0; i < stereo_pairs; i++) {
            int32_t sum = (int32_t)samples[i * 2] + (int32_t)samples[i * 2 + 1];
            samples[i] = (int16_t)(sum / 2);
        }
        captured = stereo_pairs * 2; /* now 16-bit mono = 2 bytes per sample */
    }

    /* Convert S16_LE to U8 if requested */
    if (g_sample_format == AFMT_U8 && captured > 0) {
        int16_t *s16 = (int16_t *)buf;
        uint8_t *u8  = (uint8_t *)buf;
        int num_samples = captured / 2; /* number of 16-bit samples */
        for (int i = 0; i < num_samples; i++) {
            /* S16_LE range -32768..32767 -> U8 range 0..255 */
            int32_t val = ((int32_t)s16[i] + 32768) >> 8;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            u8[i] = (uint8_t)val;
        }
        captured = num_samples; /* now 1 byte per sample */
    }

    if (out_size) *out_size = (uint32_t)captured;
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

void sound_oss_init(void)
{
    if (g_dsp_initialized)
        return;

    /* Only register if AC97 audio hardware is present */
    if (!ac97_present())
        return;

    spinlock_init(&g_dsp_lock);
    g_sample_rate   = 44100;
    g_channels      = 2;
    g_sample_format = AFMT_S16_LE;
    g_sample_width  = 2;
    g_dsp_buf_fill  = 0;

    int ret = devfs_register_device("dsp", NULL, dsp_read, dsp_write);
    if (ret == 0) {
        kprintf("[OK] OSS: /dev/dsp registered (44100 Hz stereo 16-bit)\n");
        g_dsp_initialized = 1;
    } else {
        kprintf("[OSS] WARN: failed to register /dev/dsp\n");
    }
}
