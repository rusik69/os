/*
 * sound_pcm.c — PCM DMA engine: buffer management (periods/fragments)
 *
 * Implements a ring-buffer-based PCM audio buffer manager using the
 * OSS fragment model.  The buffer is divided into N fragments (periods)
 * of equal size (power of 2).  The hardware DMA engine processes one
 * fragment at a time and signals completion via interrupt.
 *
 * Key design points:
 *   - Fragment size is rounded up to a power of two at open time.
 *   - Total buffer size = fragment_size * fragment_count (power of 2).
 *   - Read/write cursors use bitmask modulo for O(1) wrap-around.
 *   - Fragment tracking uses simple integer indices, not modulo,
 *     to avoid ambiguity between "buffer full" and "buffer empty".
 *   - All public operations are thread-safe via spinlock.
 *
 * Item 372 — PCM buffer management (D142 task 1)
 */
#include "sound_pcm.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── Internal helpers ───────────────────────────────────────────────── */

/**
 * Round a fragment size to a valid power of 2 within [SOUND_PCM_FRAG_MIN,
 * SOUND_PCM_FRAG_MAX].
 */
static uint32_t sanitise_frag_size(uint32_t frag_size)
{
    if (frag_size < SOUND_PCM_FRAG_MIN)
        frag_size = SOUND_PCM_FRAG_MIN;
    if (frag_size > SOUND_PCM_FRAG_MAX)
        frag_size = SOUND_PCM_FRAG_MAX;
    return sound_pcm_roundup_pow2(frag_size);
}

/**
 * Clamp fragment count to [SOUND_PCM_FRAG_COUNT_MIN, SOUND_PCM_FRAG_COUNT_MAX].
 */
static uint32_t sanitise_frag_count(uint32_t frag_count)
{
    if (frag_count < SOUND_PCM_FRAG_COUNT_MIN)
        frag_count = SOUND_PCM_FRAG_COUNT_MIN;
    if (frag_count > SOUND_PCM_FRAG_COUNT_MAX)
        frag_count = SOUND_PCM_FRAG_COUNT_MAX;
    /* Round to power of 2 for clean masking */
    return sound_pcm_roundup_pow2(frag_count);
}

/* ── Stream lifecycle ───────────────────────────────────────────────── */

int sound_pcm_init_stream(struct sound_pcm_stream *stream,
                           enum sound_pcm_direction dir,
                           uint32_t frag_size,
                           uint32_t frag_count,
                           void *buf)
{
    if (!stream)
        return -EINVAL;

    /* Sanitise parameters */
    frag_size  = sanitise_frag_size(frag_size);
    frag_count = sanitise_frag_count(frag_count);

    /* Total buffer size must be a power of two for bitmask modulo */
    uint32_t buf_size = frag_size * frag_count;
    if (buf_size < frag_size || buf_size < frag_count) {
        /* Overflow */
        return -EINVAL;
    }
    /* Make sure the total is a power of 2 as well */
    uint32_t round_buf = sound_pcm_roundup_pow2(buf_size);
    if (round_buf != buf_size) {
        /* Extend to next power of 2 */
        buf_size = round_buf;
    }

    /* Allocate or adopt buffer */
    if (buf) {
        stream->buffer           = (uint8_t *)buf;
        stream->buffer_alloc_base = NULL;  /* not owned, no free needed */
        stream->buffer_owned     = 0;
    } else {
        /* Overallocate by DMA_ALIGN-1 to guarantee cache-line alignment */
        void *alloc = kmalloc(buf_size + SOUND_PCM_DMA_ALIGN - 1);
        if (!alloc)
            return -ENOMEM;
        stream->buffer_alloc_base = alloc;
        stream->buffer = (uint8_t *)(((uintptr_t)alloc
                                      + SOUND_PCM_DMA_ALIGN - 1)
                                     & ~(uintptr_t)(SOUND_PCM_DMA_ALIGN - 1));
        stream->buffer_owned = 1;
    }

    /* Initialise fields */
    stream->frag_size       = frag_size;
    stream->frag_count      = frag_count;
    stream->buf_size        = buf_size;
    stream->buf_mask        = buf_size - 1;
    stream->hw_ptr          = 0;
    stream->app_ptr         = 0;
    stream->frag_hw         = 0;
    stream->frag_app        = 0;
    stream->active          = 0;
    stream->underrun        = 0;
    stream->overrun         = 0;
    stream->draining        = 0;
    stream->dir             = dir;
    stream->fragment_done   = NULL;
    stream->fragment_priv   = NULL;

    spinlock_init(&stream->lock);

    /* Clear the buffer */
    memset(stream->buffer, 0, buf_size);

    return 0;
}

void sound_pcm_destroy_stream(struct sound_pcm_stream *stream)
{
    if (!stream)
        return;

    if (stream->buffer_owned && stream->buffer_alloc_base) {
        kfree(stream->buffer_alloc_base);
    }
    stream->buffer           = NULL;
    stream->buffer_alloc_base = NULL;
    stream->buffer_owned = 0;
    stream->active       = 0;
}

/* ── Ring buffer cursor arithmetic ──────────────────────────────────── */

/**
 * Return the number of bytes that can be written (app_ptr -> hw_ptr
 * within the ring).  For playback, this is the empty space.
 * For capture, this is the occupied space.
 */
static uint32_t avail_write_locked(struct sound_pcm_stream *s)
{
    /* How much space is between hw_ptr and app_ptr?
     * For playback: app produces (app_ptr), DMA consumes (hw_ptr).
     *   filled = app_ptr - hw_ptr = data in buffer,
     *   free   = buf_size - filled.
     * For capture: DMA produces (hw_ptr), app consumes (app_ptr).
     *   filled = hw_ptr - app_ptr = data in buffer,
     *   free   = buf_size - filled.
     */
    uint32_t filled;
    if (s->dir == SOUND_PCM_PLAYBACK) {
        filled = s->app_ptr - s->hw_ptr;
    } else {
        filled = s->hw_ptr - s->app_ptr;
    }
    if (filled > s->buf_size) {
        /* Should not happen, but clamp */
        return 0;
    }
    return s->buf_size - filled;
}

/**
 * Return the number of bytes available for reading.
 * For playback: data available for DMA = app_ptr - hw_ptr.
 * For capture: data available for app = app_ptr - hw_ptr (hw_ptr is where
 * DMA wrote, app_ptr is where app read).
 */
static uint32_t avail_read_locked(struct sound_pcm_stream *s)
{
    /* How much data is available for the consumer to read.
     * For playback: DMA is the consumer of data produced by the app.
     *   filled = app_ptr - hw_ptr (data not yet consumed by DMA).
     * For capture: app is the consumer of data produced by DMA.
     *   filled = hw_ptr - app_ptr (data not yet read by the app).
     */
    uint32_t filled;
    if (s->dir == SOUND_PCM_PLAYBACK) {
        filled = s->app_ptr - s->hw_ptr;
    } else {
        filled = s->hw_ptr - s->app_ptr;
    }
    if (filled > s->buf_size)
        return 0;
    return filled;
}

/* ── Public API (thread-safe wrappers) ──────────────────────────────── */

uint32_t sound_pcm_get_avail_write(struct sound_pcm_stream *stream)
{
    if (!stream)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);
    uint32_t avail = avail_write_locked(stream);
    spinlock_irqsave_release(&stream->lock, irq_flags);
    return avail;
}

uint32_t sound_pcm_get_avail_read(struct sound_pcm_stream *stream)
{
    if (!stream)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);
    uint32_t avail = avail_read_locked(stream);
    spinlock_irqsave_release(&stream->lock, irq_flags);
    return avail;
}

/* ── Read / Write ───────────────────────────────────────────────────── */

int sound_pcm_write(struct sound_pcm_stream *stream,
                    const void *data, uint32_t size)
{
    if (!stream || !data)
        return -EINVAL;
    if (stream->dir != SOUND_PCM_PLAYBACK)
        return -EPERM;
    if (size == 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    uint32_t avail = avail_write_locked(stream);
    if (avail == 0) {
        stream->overrun = 1;
        spinlock_irqsave_release(&stream->lock, irq_flags);
        return 0; /* OSS semantics: return 0 on full buffer */
    }

    uint32_t to_write = (size < avail) ? size : avail;
    uint32_t written  = 0;
    const uint8_t *src = (const uint8_t *)data;

    while (written < to_write) {
        uint32_t offset = stream->app_ptr & stream->buf_mask;
        uint32_t chunk  = to_write - written;
        uint32_t space  = stream->buf_size - offset;
        if (chunk > space)
            chunk = space;

        memcpy(stream->buffer + offset, src + written, chunk);
        stream->app_ptr += chunk;
        written += chunk;
    }

    /* Update fragment tracking */
    uint32_t new_frag_app = stream->app_ptr / stream->frag_size;
    if (new_frag_app > stream->frag_app) {
        stream->frag_app = new_frag_app;
    }

    stream->active  = 1;
    stream->underrun = 0;

    spinlock_irqsave_release(&stream->lock, irq_flags);
    return (int)written;
}

int sound_pcm_read(struct sound_pcm_stream *stream,
                   void *buf, uint32_t max_size)
{
    if (!stream || !buf)
        return -EINVAL;
    if (stream->dir != SOUND_PCM_CAPTURE)
        return -EPERM;
    if (max_size == 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    uint32_t avail = avail_read_locked(stream);
    if (avail == 0) {
        spinlock_irqsave_release(&stream->lock, irq_flags);
        return 0;
    }

    uint32_t to_read = (max_size < avail) ? max_size : avail;
    uint32_t readb  = 0;
    uint8_t *dst    = (uint8_t *)buf;

    while (readb < to_read) {
        uint32_t offset = stream->app_ptr & stream->buf_mask;
        uint32_t chunk  = to_read - readb;
        uint32_t space  = stream->buf_size - offset;
        if (chunk > space)
            chunk = space;

        memcpy(dst + readb, stream->buffer + offset, chunk);
        stream->app_ptr += chunk;
        readb += chunk;
    }

    /* Update fragment tracking for capture */
    uint32_t new_frag_app = stream->app_ptr / stream->frag_size;
    if (new_frag_app > stream->frag_app) {
        stream->frag_app = new_frag_app;
    }

    spinlock_irqsave_release(&stream->lock, irq_flags);
    return (int)readb;
}

/* ── Buffer info (OSS GETOSPACE / GETISPACE) ────────────────────────── */

void sound_pcm_get_ospace(struct sound_pcm_stream *stream,
                          struct sound_pcm_buf_info *info)
{
    if (!stream || !info)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    uint32_t avail = avail_write_locked(stream);
    info->fragstotal = (int)stream->frag_count;
    info->fragsize   = (int)stream->frag_size;
    info->fragments  = (int)(avail / stream->frag_size);
    info->bytes      = (int)avail;

    spinlock_irqsave_release(&stream->lock, irq_flags);
}

void sound_pcm_get_ispace(struct sound_pcm_stream *stream,
                          struct sound_pcm_buf_info *info)
{
    if (!stream || !info)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    uint32_t avail = avail_read_locked(stream);
    info->fragstotal = (int)stream->frag_count;
    info->fragsize   = (int)stream->frag_size;
    info->fragments  = (int)(avail / stream->frag_size);
    info->bytes      = (int)avail;

    spinlock_irqsave_release(&stream->lock, irq_flags);
}

/* ── DMA engine interface ───────────────────────────────────────────── */

uint32_t sound_pcm_dma_get_fragment(struct sound_pcm_stream *stream,
                                    void **out_ptr)
{
    if (!stream || !out_ptr) {
        if (out_ptr) *out_ptr = NULL;
        return 0;
    }

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    /* Determine which fragment the DMA engine should process next */
    uint32_t current_frag = stream->hw_ptr / stream->frag_size;

    if (stream->dir == SOUND_PCM_PLAYBACK) {
        /* Playback: DMA reads from buffer. Check if there's data. */
        uint32_t filled = avail_read_locked(stream);
        if (filled < stream->frag_size) {
            /* Not enough data for a full fragment */
            *out_ptr = NULL;
            spinlock_irqsave_release(&stream->lock, irq_flags);
            return 0;
        }

        uint32_t frag_offset = (current_frag * stream->frag_size) & stream->buf_mask;
        *out_ptr = (void *)(stream->buffer + frag_offset);
        uint32_t ret = stream->frag_size;
        spinlock_irqsave_release(&stream->lock, irq_flags);
        return ret;
    } else {
        /* Capture: DMA writes into buffer. Find a free fragment slot. */
        uint32_t filled = avail_read_locked(stream);
        uint32_t total  = stream->buf_size;
        uint32_t free   = total - filled;
        if (free < stream->frag_size) {
            /* No free fragment slot */
            *out_ptr = NULL;
            spinlock_irqsave_release(&stream->lock, irq_flags);
            return 0;
        }

        uint32_t frag_offset = (current_frag * stream->frag_size) & stream->buf_mask;
        *out_ptr = (void *)(stream->buffer + frag_offset);
        uint32_t ret = stream->frag_size;
        spinlock_irqsave_release(&stream->lock, irq_flags);
        return ret;
    }
}

void sound_pcm_dma_consume(struct sound_pcm_stream *stream)
{
    if (!stream)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    /* Advance the hardware pointer by one fragment */
    stream->hw_ptr += stream->frag_size;
    stream->frag_hw = stream->hw_ptr / stream->frag_size;

    /* If all data has been consumed in playback mode, mark inactive */
    if (stream->dir == SOUND_PCM_PLAYBACK) {
        uint32_t filled = avail_read_locked(stream);
        if (filled == 0) {
            stream->active = 0;
        }
    }

    /* Fire the fragment completion callback */
    void (*cb)(void *) = stream->fragment_done;
    void *priv = stream->fragment_priv;

    spinlock_irqsave_release(&stream->lock, irq_flags);

    if (cb)
        cb(priv);
}

/* ── Reset / Drain ──────────────────────────────────────────────────── */

void sound_pcm_reset(struct sound_pcm_stream *stream)
{
    if (!stream)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);

    stream->hw_ptr    = 0;
    stream->app_ptr   = 0;
    stream->frag_hw   = 0;
    stream->frag_app  = 0;
    stream->active    = 0;
    stream->underrun  = 0;
    stream->overrun   = 0;
    stream->draining  = 0;

    if (stream->buffer)
        memset(stream->buffer, 0, stream->buf_size);

    spinlock_irqsave_release(&stream->lock, irq_flags);
}

void sound_pcm_drain(struct sound_pcm_stream *stream)
{
    if (!stream)
        return;

    /* For now, just mark draining and wait for DMA to empty the buffer.
     * A full implementation would sleep until the hw_ptr catches up. */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&stream->lock, &irq_flags);
    stream->draining = 1;
    spinlock_irqsave_release(&stream->lock, irq_flags);

    /* TODO: Implement blocking drain with waitqueue */
    kprintf("[sound_pcm] drain: not fully implemented yet\n");
}

/* ── Module metadata ────────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("PCM DMA engine: buffer management with period/fragment support");
MODULE_AUTHOR("1000 Changes Project");
