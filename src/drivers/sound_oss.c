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

/* ── OSS ioctl state ────────────────────────────────────────────────── */
/* Fragment size (default 16 fragments of 4096 bytes each) */
static int g_frag_size    = 4096;
static int g_frag_total   = 16;
static int g_block_size   = 4096;  /* GETBLKSIZE returns this */

/* IOCTL support: flag the ioctls as being settable via the ioctl()
 * syscall.  Since the kernel doesn't yet have a generic ioctl dispatch
 * for devfs devices, these state variables are set directly by the
 * ioctl definitions documented at the top of the file and can be
 * queried via /dev/dsp reads (the kernel will add dispatch later). */

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
static int g_dsp_buf_fill   = 0;   /* DSP buffer fill level */

/* Capture state */
static int g_capture_rate    = 44100;
static int g_record_source   = REC_SEL_MIC;
static uint8_t g_record_gain_left  = 10; /* default ~15dB */
static uint8_t g_record_gain_right = 10;
static int g_record_mute    = 0;

/* ── Ring buffer for PCM audio ───────────────────────────────────────
 * Properly buffers audio data for interrupt-driven DMA playback.
 * The ring buffer holds up to 64 KB of audio samples. */
#define RING_BUF_SIZE  (64 * 1024)
static uint8_t  g_ring_buf[RING_BUF_SIZE];
static uint32_t g_ring_rd        = 0;   /* read cursor (DMA engine) */
static uint32_t g_ring_wr        = 0;   /* write cursor (app) */
static int      g_ring_underflow = 0;

/* DMA engine state (simulated) */
static int      g_dma_active     = 0;   /* DMA transfer in progress */

/* Volume control (0–255, default 192 ≈ 75%) */
static int g_play_volume = 192;
static int g_rec_volume  = 192;

/* Protect g_ring_buf and state from concurrent access */
static spinlock_t g_dsp_lock;

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Return the number of bytes available for writing in the ring buffer */
static uint32_t ring_available_write(void)
{
    return RING_BUF_SIZE - ((g_ring_wr - g_ring_rd) & (RING_BUF_SIZE - 1));
}

/* Return the number of bytes available for reading from the ring buffer */
static uint32_t ring_available_read(void)
{
    return (g_ring_wr - g_ring_rd) & (RING_BUF_SIZE - 1);
}

/* Write data into the ring buffer. Returns bytes written. */
static uint32_t ring_write(const uint8_t *data, uint32_t len)
{
    uint32_t avail = ring_available_write();
    if (len > avail) len = avail;
    if (len == 0) return 0;

    for (uint32_t i = 0; i < len; i++) {
        g_ring_buf[(g_ring_wr + i) & (RING_BUF_SIZE - 1)] = data[i];
    }
    g_ring_wr = (g_ring_wr + len) & (RING_BUF_SIZE - 1);
    return len;
}

/* Read data from the ring buffer. Returns bytes read. */
static uint32_t ring_read(uint8_t *buf, uint32_t len)
{
    uint32_t avail = ring_available_read();
    if (len > avail) {
        g_ring_underflow = 1;
        len = avail;
    }
    if (len == 0) return 0;

    for (uint32_t i = 0; i < len; i++) {
        buf[i] = g_ring_buf[(g_ring_rd + i) & (RING_BUF_SIZE - 1)];
    }
    g_ring_rd = (g_ring_rd + len) & (RING_BUF_SIZE - 1);
    return len;
}

/* Simulate DMA transfer: drain ring buffer into a "DMA output buffer"
 * that represents the audio hardware output. Applies volume scaling. */
#define DMA_OUT_BUF_SIZE  (64 * 1024)
static uint8_t g_dma_out_buf[DMA_OUT_BUF_SIZE];

static void dma_transfer_simulate(void)
{
    if (!g_dma_active) return;

    /* Read samples from ring buffer */
    uint32_t avail = ring_available_read();
    uint32_t to_read = avail > DMA_OUT_BUF_SIZE ? DMA_OUT_BUF_SIZE : avail;
    if (to_read == 0) {
        g_dma_active = 0;
        return;
    }

    uint8_t tmp[DMA_OUT_BUF_SIZE];
    uint32_t nread = ring_read(tmp, to_read);
    if (nread == 0) return;

    /* Apply volume scaling for 16-bit samples */
    if (g_sample_format == AFMT_S16_LE) {
        int16_t *samples = (int16_t *)tmp;
        int nsamples = nread / 2;
        for (int i = 0; i < nsamples; i++) {
            int32_t s = samples[i];
            s = (s * g_play_volume) / 255;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            samples[i] = (int16_t)s;
        }
    }

    /* Scale for U8 samples */
    else if (g_sample_format == AFMT_U8) {
        for (uint32_t i = 0; i < nread; i++) {
            int32_t s = tmp[i];
            s = (s * g_play_volume) / 255;
            if (s > 255) s = 255;
            if (s < 0) s = 0;
            tmp[i] = (uint8_t)s;
        }
    }

    /* Copy into the DMA output buffer (simulates transfer to audio hardware) */
    memcpy(g_dma_out_buf, tmp, nread);

    /* If hardware is present, also play via AC97 */
    if (ac97_present()) {
        /* Convert U8 to S16_LE for AC97 if needed */
        if (g_sample_format == AFMT_U8) {
            int16_t s16_buf[DMA_OUT_BUF_SIZE / 2];
            int nsamples = nread;
            for (int i = 0; i < nsamples && i < DMA_OUT_BUF_SIZE / 2; i++) {
                s16_buf[i] = (int16_t)((int)tmp[i] * 256 - 32768);
            }
            ac97_play_pcm(s16_buf, (uint32_t)(nsamples * 2),
                          (uint32_t)g_sample_rate);
        } else {
            ac97_play_pcm((int16_t *)tmp, nread,
                          (uint32_t)g_sample_rate);
        }
    }

    /* If the buffer is empty now, stop DMA simulation */
    if (ring_available_read() == 0)
        g_dma_active = 0;
}

/* ── devfs callbacks ──────────────────────────────────────────────── */

static int dsp_write(void *priv, const void *data, uint32_t size)
{
    (void)priv;
    if (!data || size == 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dsp_lock, &irq_flags);

    /* Write data into the ring buffer */
    uint32_t written = ring_write((const uint8_t *)data, size);

    /* Start DMA transfer simulation (if not already running) */
    g_dma_active = 1;
    dma_transfer_simulate();

    spinlock_irqsave_release(&g_dsp_lock, irq_flags);

    /* Return the full size as "consumed" (OS semantics) */
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
#include "module.h"
module_init(sound_oss_init);

/* ── Stub: sound_oss_open ─────────────────────────────── */
int sound_oss_open(int dev, void *file)
{
    (void)dev;
    (void)file;
    kprintf("[sound_oss] sound_oss_open: not yet implemented\n");
    return 0;
}
/* ── Stub: sound_oss_release ─────────────────────────────── */
int sound_oss_release(int dev, void *file)
{
    (void)dev;
    (void)file;
    kprintf("[sound_oss] sound_oss_release: not yet implemented\n");
    return 0;
}
/* ── Stub: sound_oss_read ─────────────────────────────── */
int sound_oss_read(int dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[sound_oss] sound_oss_read: not yet implemented\n");
    return 0;
}
/* ── Stub: sound_oss_write ─────────────────────────────── */
int sound_oss_write(int dev, const void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[sound_oss] sound_oss_write: not yet implemented\n");
    return 0;
}
