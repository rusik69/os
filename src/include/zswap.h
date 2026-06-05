#ifndef ZSWAP_H
#define ZSWAP_H

#include "types.h"

/*
 * ── zswap — Compressed in-memory swap cache (Item 224) ────────────────
 *
 * Provides a compressed cache for swapped-out pages, reducing swap I/O.
 * When a page is swapped out, zswap attempts to compress it and store the
 * compressed data in a memory pool instead of (or in addition to) writing
 * it to the swap device.  On swap-in, the compressed page is decompressed
 * from the pool, avoiding a disk read.
 *
 * If compression fails (incompressible data) or the pool is full, the
 * page is written to the swap device as usual.
 *
 * Uses the zcomp compression framework for per-CPU stream compression
 * and supports multiple compression algorithms (default: "fast" LZ77).
 */

/* ── Constants ──────────────────────────────────────────────────────── */

/** Maximum number of entries in the zswap hash table. */
#define ZSWAP_HASH_SIZE 256

/** Default maximum pool size: percentage of total RAM. */
#define ZSWAP_DEFAULT_POOL_PCT 5

/** Minimum pool size in KB (when ZSWAP_DEFAULT_POOL_PCT yields very small). */
#define ZSWAP_MIN_POOL_KB 1024

/** Maximum compressed data per page (LZ77 worst case ~12.5% expansion). */
#define ZSWAP_MAX_COMP_LEN 4608

/* ── Public API ─────────────────────────────────────────────────────── */

/** Initialize the zswap subsystem. Called once at boot. */
void zswap_init(void);

/**
 * zswap_store - Attempt to compress and store a swapped-out page.
 * @phys_addr:  Physical address of the 4K page to store.
 * @dev_idx:    Swap device index (for keying in hash table).
 * @slot:       Swap slot index (for keying in hash table).
 *
 * Compresses the page using the configured algorithm.  If compression
 * succeeds and the pool isn't full, stores the compressed data in the
 * hash table and returns 0.  On failure (incompressible, pool full,
 * ENOMEM) returns -1 so the caller falls through to disk I/O.
 *
 * Must be called before the swap slot bitmap is updated (so that
 * zswap_load can find the entry if swap_in is called later).
 */
int zswap_store(uint64_t phys_addr, int dev_idx, uint32_t slot);

/**
 * zswap_load - Attempt to decompress a page from the zswap pool.
 * @dev_idx:    Swap device index.
 * @slot:       Swap slot index.
 * @phys_addr:  Physical address to write decompressed data into.
 *
 * Looks up (dev_idx, slot) in the hash table.  If found, decompresses
 * the data into the physical page and removes the entry from the pool.
 * Returns 0 on success, -1 if not found (caller should read from disk).
 */
int zswap_load(int dev_idx, uint32_t slot, uint64_t phys_addr);

/**
 * zswap_free - Remove a zswap entry (e.g., on swap slot free).
 * @dev_idx:  Swap device index.
 * @slot:     Swap slot index.
 *
 * Removes the entry from the hash table and frees the compressed buffer.
 * Called by swap_free_slot to keep the pool in sync.
 */
void zswap_free(int dev_idx, uint32_t slot);

/**
 * zswap_stats - Query zswap statistics.
 * @out_pages:    Receives number of pages currently stored.
 * @out_size_kb:  Receives total compressed size in KB.
 */
void zswap_stats(uint32_t *out_pages, uint32_t *out_size_kb);

/**
 * zswap_is_full - Check whether the pool is over capacity.
 * Returns 1 if the pool should reject new entries, 0 otherwise.
 */
int zswap_is_full(void);

#endif /* ZSWAP_H */
