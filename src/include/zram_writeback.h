#ifndef ZRAM_WRITEBACK_H
#define ZRAM_WRITEBACK_H

#include "types.h"

/*
 * zram_writeback — write cold compressed pages to a backing device
 *
 * When a backing device (e.g., an SSD partition) is configured, zram
 * can decompress cold pages and store them on the backing device to
 * relieve memory pressure when compression ratio is high.
 */

/* Maximum backing device path length */
#define ZRAM_BACKING_DEV_PATH_MAX 64

/* Maximum writeback rate limit (kB/s), 0 = unlimited */
#define ZRAM_WRITEBACK_LIMIT_MAX  1048576U  /* 1 GB/s */

/* ── Backing device configuration ──────────────────────────────────── */

/* Set the backing device path (e.g., "/dev/sda1").
 * Returns 0 on success, -errno on failure. */
int zram_set_backing_dev(const char *path);

/* Get the current backing device path (empty string if none). */
const char *zram_get_backing_dev(void);

/* ── Writeback rate limiting ────────────────────────────────────────── */

/* Set writeback rate limit in kB/s (0 = unlimited). */
int zram_set_writeback_limit(uint32_t limit_kbps);

/* Get current writeback rate limit in kB/s. */
uint32_t zram_get_writeback_limit(void);

/* ── LRU tracking ──────────────────────────────────────────────────── */

/* Mark a zram slot as recently accessed (call on read/write). */
void zram_writeback_mark_accessed(uint64_t slot_index);

/* ── Writeback operations ──────────────────────────────────────────── */

/*
 * zram_writeback_store() — Decompress a zram page, write it to the
 * backing device at the given offset, then free the compressed slot.
 * Returns 0 on success, -errno on failure.
 *
 * @slot_index:  Index of the zram slot to write back
 * @backing_off: Offset in the backing device (in sectors)
 */
int zram_writeback_store(uint64_t slot_index, uint64_t backing_off);

/*
 * zram_writeback_read() — Read a page from the backing device at the
 * given offset, compress it into a new zram slot, free the backing slot.
 * Returns 0 on success, -errno on failure.
 *
 * @slot_index:  Index of the zram slot to fill
 * @backing_off: Offset in the backing device (in sectors)
 */
int zram_writeback_read(uint64_t slot_index, uint64_t backing_off);

/*
 * Try to write back one cold page from the LRU list.
 * Returns 1 if a page was written back, 0 if nothing to write back,
 * negative errno on error.
 */
int zram_writeback_evict_one(void);

/* ── Initialisation ────────────────────────────────────────────────── */

/* Initialise the zram writeback subsystem. */
void zram_writeback_init(void);

#endif /* ZRAM_WRITEBACK_H */
