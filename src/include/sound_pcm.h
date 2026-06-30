/*
 * sound_pcm.h — PCM DMA engine: buffer management (periods/fragments)
 *
 * Implements a generic PCM buffer abstraction using the OSS fragment
 * model: a ring buffer subdivided into fragments (periods), where each
 * fragment triggers a hardware interrupt when filled/emptied.
 *
 * Fragment size and count are configurable at open time and must be
 * powers of two.  All internal arithmetic uses bitmask modulo for
 * efficiency.
 *
 * This layer sits between the OSS /dev/dsp character device and the
 * hardware-specific DMA drivers (AC97, etc.).
 *
 * Item 372 — PCM buffer management (D142 task 1)
 */
#ifndef SOUND_PCM_H
#define SOUND_PCM_H

#include "types.h"
#include "spinlock.h"

/* ── Constants ─────────────────────────────────────────────────────── */

/** Minimum fragment size (bytes) */
#define SOUND_PCM_FRAG_MIN      64U

/** Maximum fragment size (bytes) */
#define SOUND_PCM_FRAG_MAX      65536U

/** Minimum number of fragments */
#define SOUND_PCM_FRAG_COUNT_MIN    2U

/** Maximum number of fragments */
#define SOUND_PCM_FRAG_COUNT_MAX    64U

/** Default fragment size (4096 bytes) */
#define SOUND_PCM_FRAG_DEFAULT      4096U

/** Default fragment count (16) */
#define SOUND_PCM_FRAG_COUNT_DEFAULT 16U

/** PCM stream directions */
enum sound_pcm_direction {
    SOUND_PCM_PLAYBACK = 0,   /**< Output (application -> DMA -> codec) */
    SOUND_PCM_CAPTURE  = 1,   /**< Input  (codec -> DMA -> application) */
};

/* ── Buffer info (mimics OSS audio_buf_info) ────────────────────────── */

struct sound_pcm_buf_info {
    int fragments;        /**< Number of fragments currently available */
    int fragstotal;       /**< Total number of fragments */
    int fragsize;         /**< Size of each fragment in bytes */
    int bytes;            /**< Total bytes available */
};

/* ── PCM stream state ───────────────────────────────────────────────── */

/** Per-stream PCM buffer state */
struct sound_pcm_stream {
    /* Configuration (set at open time, immutable while active) */
    uint32_t    frag_size;       /**< Fragment size in bytes (power of 2) */
    uint32_t    frag_count;      /**< Total number of fragments */
    uint32_t    buf_size;        /**< Total buffer size (frag_size * frag_count) */
    uint32_t    buf_mask;        /**< Buffer size - 1 (for modulo arithmetic) */

    /* Ring buffer */
    uint8_t    *buffer;          /**< Allocated ring buffer storage */
    int         buffer_owned;    /**< 1 if we allocated buffer ourselves */

    /* Cursor tracking */
    volatile uint32_t  hw_ptr;   /**< Hardware (DMA) pointer — bytes consumed */
    volatile uint32_t  app_ptr;  /**< Application pointer — bytes produced */

    /* Fragment tracking */
    volatile uint32_t  frag_hw;  /**< Fragment index last processed by DMA */
    volatile uint32_t  frag_app; /**< Fragment index last written by app */

    /* Stream state */
    int         active;          /**< 1 if DMA is running */
    int         underrun;        /**< 1 if underrun occurred */
    int         overrun;         /**< 1 if overrun occurred */
    int         draining;        /**< 1 if drain requested (playback only) */

    /* Direction */
    enum sound_pcm_direction dir;

    /* Synchronisation */
    spinlock_t  lock;

    /* Callbacks (optional) — called from interrupt context */
    void (*fragment_done)(void *priv); /**< Called when a fragment completes */
    void *fragment_priv;               /**< Private data for callback */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * sound_pcm_init_stream — Initialise a PCM stream with given parameters.
 *
 * @stream:    Stream state structure (caller-allocated).
 * @dir:       Playback or capture direction.
 * @frag_size: Fragment size in bytes (will be rounded up to power of 2).
 * @frag_count: Number of fragments (will be clamped).
 * @buf:       Optional pre-allocated buffer (must be frag_size * frag_count).
 *             If NULL, memory is allocated internally via kmalloc.
 *
 * Returns 0 on success, or a negative errno on failure.
 */
int sound_pcm_init_stream(struct sound_pcm_stream *stream,
                          enum sound_pcm_direction dir,
                          uint32_t frag_size,
                          uint32_t frag_count,
                          void *buf);

/**
 * sound_pcm_destroy_stream — Release resources for a PCM stream.
 */
void sound_pcm_destroy_stream(struct sound_pcm_stream *stream);

/**
 * sound_pcm_write — Write audio data to the playback buffer.
 *
 * @stream:  Playback stream.
 * @data:    Audio data to write.
 * @size:    Number of bytes to write.
 *
 * Returns the number of bytes actually written (may be less than @size
 * if the buffer is full), or a negative errno.
 */
int sound_pcm_write(struct sound_pcm_stream *stream,
                    const void *data, uint32_t size);

/**
 * sound_pcm_read — Read audio data from the capture buffer.
 *
 * @stream:   Capture stream.
 * @buf:      Destination buffer.
 * @max_size: Maximum bytes to read.
 *
 * Returns the number of bytes actually read, or a negative errno.
 */
int sound_pcm_read(struct sound_pcm_stream *stream,
                   void *buf, uint32_t max_size);

/**
 * sound_pcm_get_ospace — Get output buffer space info (for OSS GETOSPACE).
 */
void sound_pcm_get_ospace(struct sound_pcm_stream *stream,
                          struct sound_pcm_buf_info *info);

/**
 * sound_pcm_get_ispace — Get input buffer space info (for OSS GETISPACE).
 */
void sound_pcm_get_ispace(struct sound_pcm_stream *stream,
                          struct sound_pcm_buf_info *info);

/**
 * sound_pcm_get_avail_write — Return bytes available for writing.
 */
uint32_t sound_pcm_get_avail_write(struct sound_pcm_stream *stream);

/**
 * sound_pcm_get_avail_read — Return bytes available for reading.
 */
uint32_t sound_pcm_get_avail_read(struct sound_pcm_stream *stream);

/**
 * sound_pcm_dma_consume — Called by DMA engine after transferring a fragment.
 *
 * Advances the hardware pointer and fires the fragment_done callback.
 * Must be called from the DMA interrupt handler or softirq context.
 */
void sound_pcm_dma_consume(struct sound_pcm_stream *stream);

/**
 * sound_pcm_dma_get_fragment — Get pointer and size of next fragment for DMA.
 *
 * @stream:  Stream to query.
 * @out_ptr: Receives pointer to the fragment data.
 *
 * Returns the fragment size in bytes, or 0 if no fragment is available.
 * For playback: returns data that the DMA engine should transfer to the codec.
 * For capture:  returns space where the DMA engine should write captured data.
 */
uint32_t sound_pcm_dma_get_fragment(struct sound_pcm_stream *stream,
                                    void **out_ptr);

/**
 * sound_pcm_reset — Reset a PCM stream (discards buffered data).
 */
void sound_pcm_reset(struct sound_pcm_stream *stream);

/**
 * sound_pcm_drain — Wait for playback buffer to drain (blocking).
 * Not yet implemented — for future use.
 */
void sound_pcm_drain(struct sound_pcm_stream *stream);

/**
 * sound_pcm_is_power_of_2 — Check if a value is a power of two.
 */
static inline int sound_pcm_is_power_of_2(uint32_t v)
{
    return v && (v & (v - 1)) == 0;
}

/**
 * sound_pcm_roundup_pow2 — Round up to the next power of two.
 */
static inline uint32_t sound_pcm_roundup_pow2(uint32_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

#endif /* SOUND_PCM_H */
