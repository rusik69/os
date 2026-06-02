#ifndef ZCOMP_H
#define ZCOMP_H

#include "types.h"

/*
 * ── Compression Algorithm Abstraction ────────────────────────────────
 *
 * Provides a unified interface for multiple compression algorithms,
 * with per-CPU stream support for lock-free concurrent compression.
 *
 * Designed for ZRAM page compression (4K pages) but usable elsewhere.
 *
 * Algorithms:
 *   ZCOMP_ALGO_FAST  — Fast LZ77 hash-chain compressor (default)
 *   ZCOMP_ALGO_LZSS  — Higher-ratio LZSS compressor
 *   ZCOMP_ALGO_NONE  — Pass-through (no compression)
 */

/* ── Algorithm identifiers ─────────────────────────────────────────── */
#define ZCOMP_ALGO_FAST    0
#define ZCOMP_ALGO_LZSS    1
#define ZCOMP_ALGO_NONE    2

/* Max number of registered algorithms */
#define ZCOMP_MAX_ALGOS    8

/* Max algorithm name length (including null) */
#define ZCOMP_MAX_NAME     16

/* ── Algorithm operations ──────────────────────────────────────────── */
struct zcomp_ops {
    const char *name;          /* Human-readable name, e.g. "fast", "lzss" */
    uint32_t    algo_id;       /* One of ZCOMP_ALGO_* */

    /**
     * compress - Compress a 4K page
     * @src:        Input data (src_len bytes)
     * @src_len:    Input size (typically 4096)
     * @dst:        Output buffer
     * @dst_len:    Output buffer size
     * @workspace:  Per-CPU workspace (from create_workspace), or NULL
     *
     * Returns: Compressed size (>0) on success.
     *          Returns 0 if data is incompressible (caller stores raw).
     *          Negative errno on error.
     */
    int (*compress)(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_len,
                    void *workspace);

    /**
     * decompress - Decompress a 4K page
     * @src:        Compressed input data
     * @src_len:    Compressed input size
     * @dst:        Output buffer (must be at least PAGE_SIZE)
     * @dst_len:    Output buffer size
     * @workspace:  Per-CPU workspace (from create_workspace), or NULL
     *
     * Returns: Decompressed size (>0) on success, negative errno on error.
     */
    int (*decompress)(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_len,
                      void *workspace);

    /**
     * create_workspace - Allocate per-CPU workspace
     *
     * Returns: Opaque workspace pointer, or NULL if algorithm needs none.
     */
    void *(*create_workspace)(void);

    /**
     * destroy_workspace - Free per-CPU workspace
     * @ws: Workspace pointer (or NULL).
     */
    void (*destroy_workspace)(void *ws);
};

/* ── Per-CPU stream ────────────────────────────────────────────────── */
struct zcomp_stream {
    const struct zcomp_ops *ops;
    void                   *workspace;
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * zcomp_register - Register a compression algorithm
 * @ops: Static ops descriptor (must persist for lifetime)
 *
 * Returns: 0 on success, negative errno on error.
 */
int zcomp_register(const struct zcomp_ops *ops);

/**
 * zcomp_find - Find algorithm ops by ID
 * @algo_id: ZCOMP_ALGO_* identifier
 *
 * Returns: Ops pointer, or NULL if not found.
 */
const struct zcomp_ops *zcomp_find(uint32_t algo_id);

/**
 * zcomp_find_by_name - Find algorithm ops by name
 * @name: Algorithm name string
 *
 * Returns: Ops pointer, or NULL if not found.
 */
const struct zcomp_ops *zcomp_find_by_name(const char *name);

/**
 * zcomp_get_algo_name - Get name of algorithm by ID
 * @algo_id: Algorithm identifier
 *
 * Returns: Name string, or "unknown" if not found.
 */
const char *zcomp_get_algo_name(uint32_t algo_id);

/**
 * zcomp_streams_init - Initialize per-CPU streams for an algorithm
 * @streams:  Array of at least max_cpus stream structs
 * @max_cpus: Max CPUs to support
 * @ops:      Algorithm ops to use
 *
 * For each CPU, creates a workspace via ops->create_workspace().
 * Returns: 0 on success, negative errno on error.
 */
int zcomp_streams_init(struct zcomp_stream *streams, int max_cpus,
                       const struct zcomp_ops *ops);

/**
 * zcomp_stream_get - Get stream for current CPU
 * @streams:   Array of per-CPU streams
 * @num_cpus:  Number of configured streams
 *
 * Returns: Stream pointer for the current CPU.
 *          Falls back to CPU 0 if current CPU index >= num_cpus.
 */
struct zcomp_stream *zcomp_stream_get(struct zcomp_stream *streams,
                                      int num_cpus);

/**
 * zcomp_stream_compress - Compress using per-CPU stream
 * @zs:       Stream for current CPU (from zcomp_stream_get)
 * @src:      Input data
 * @src_len:  Input size
 * @dst:      Output buffer
 * @dst_len:  Output buffer size
 *
 * Convenience wrapper that passes workspace from stream to ops->compress.
 */
static inline int zcomp_stream_compress(struct zcomp_stream *zs,
                                         const uint8_t *src, size_t src_len,
                                         uint8_t *dst, size_t dst_len)
{
    return zs->ops->compress(src, src_len, dst, dst_len, zs->workspace);
}

/**
 * zcomp_stream_decompress - Decompress using per-CPU stream
 * @zs:       Stream for current CPU (from zcomp_stream_get)
 * @src:      Compressed input
 * @src_len:  Compressed size
 * @dst:      Output buffer
 * @dst_len:  Output buffer size
 *
 * Convenience wrapper that passes workspace from stream to ops->decompress.
 */
static inline int zcomp_stream_decompress(struct zcomp_stream *zs,
                                           const uint8_t *src, size_t src_len,
                                           uint8_t *dst, size_t dst_len)
{
    return zs->ops->decompress(src, src_len, dst, dst_len, zs->workspace);
}

/**
 * zcomp_streams_destroy - Destroy per-CPU streams
 * @streams:   Array of per-CPU streams
 * @num_cpus:  Number of streams to destroy
 *
 * Destroys each per-CPU workspace and zeros the stream entries.
 */
void zcomp_streams_destroy(struct zcomp_stream *streams, int num_cpus);

/**
 * zcomp_init - Initialize compression subsystem
 *
 * Registers all built-in compression algorithms.
 * Called once during kernel boot.
 */
void zcomp_init(void);

#endif /* ZCOMP_H */
