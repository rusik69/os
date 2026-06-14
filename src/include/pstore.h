#ifndef PSTORE_H
#define PSTORE_H

#include "types.h"
#include "notifier.h"

/* ── Persistent storage region (survives reboot) ────────────────────────
 *
 * Physical memory region: fixed at 0x70000–0x80000 (64 KB).
 * This address is in the first 1 MB (below the kernel at 0x100000),
 * already identity-mapped by the boot page tables, and never touched
 * by the physical memory manager (which manages memory above 1 MB).
 *
 * Layout:
 *   [0..31]    struct pstore_region_header
 *   [32..N)    Ring buffer of struct pstore_record (fixed-size slots)
 *
 * Each record slot is 4100 bytes (magic 4 + length 4 + timestamp 8
 * + data 4084).  With 64 KB total we fit floor((65536-32)/4100) = 15
 * slots.  When the buffer fills, the oldest record is overwritten.
 */

#define PSTORE_REGION_PADDR     0x70000ULL
#define PSTORE_REGION_SIZE      (64 * 1024)   /* 64 KB */

/* Magic numbers */
#define PSTORE_REGION_MAGIC     0x50545248U   /* "PTRH" */
#define PSTORE_RECORD_MAGIC     0x50535452U   /* "PSTR" */

/* Record types */
#define PSTORE_TYPE_DMESG       1
#define PSTORE_TYPE_PANIC       2
#define PSTORE_TYPE_OOPS        3
#define PSTORE_TYPE_EMERG       4
#define PSTORE_TYPE_DMESG_COMPRESSED  5
#define PSTORE_TYPE_PANIC_COMPRESSED  6
#define PSTORE_TYPE_OOPS_COMPRESSED   7

/* Compression threshold: records larger than this are compressed */
#define PSTORE_COMPRESS_THRESH  64

/* Maximum data payload per record (4 KB minus header overhead) */
#define PSTORE_MAX_DATA_LEN     4084

/* ── Region header (at PSTORE_REGION_PADDR) ─────────────────────────── */
struct pstore_region_header {
    uint32_t region_magic;       /* PSTORE_REGION_MAGIC */
    uint32_t version;            /* Format version (1) */
    uint32_t write_slot;         /* Next record slot index (0-based, wraps) */
    uint32_t record_count;       /* Total records since region was formatted */
    uint64_t boot_timestamp;     /* timer_get_ticks() at format time */
    uint32_t recovered;          /* 1 = previous-boot records displayed */
    uint8_t  reserved[8];        /* Padding to 32 bytes */
} __attribute__((packed));

/* ── Record header (written to ring buffer) ─────────────────────────── */
struct pstore_record {
    uint32_t magic;              /* PSTORE_RECORD_MAGIC */
    uint32_t length;             /* Bytes of valid data (0 = empty) */
    uint64_t timestamp;          /* timer_get_ticks() at write time */
    uint8_t  data[PSTORE_MAX_DATA_LEN];  /* Payload (type byte + real data) */
} __attribute__((packed));

/* Fixed slot size in the ring buffer */
#define PSTORE_RECORD_SIZE      sizeof(struct pstore_record)  /* 4100 */

/* Number of record slots in the 64 KB region (after 32-byte header) */
#define PSTORE_MAX_RECORDS      ((PSTORE_REGION_SIZE - sizeof(struct pstore_region_header)) \
                                 / PSTORE_RECORD_SIZE)  /* 15 */

/* ── Public API ──────────────────────────────────────────────────────── */

/* Initialise persistent storage.
 *   - Maps the physical region via the existing boot identity mapping.
 *   - Checks for a valid region header (previous-boot data).
 *   - If data from a prior boot exists, calls pstore_recover().
 *   - Formats a fresh header for the current boot cycle. */
void pstore_init(void);

/* Write a record of @type with @data of @len bytes to the ring buffer.
 * The type byte is prepended to the stored payload.
 * Returns 0 on success, negative on error. */
int pstore_write(uint8_t type, const uint8_t *data, int len);

/* Read a record by index (0-based, ordered newest-first).
 * @buf receives the raw data (including the leading type byte).
 * @len is the buffer size.
 * Returns number of bytes read, or negative on error. */
int pstore_read(int index, uint8_t *buf, int len);

/* Return the number of valid records stored in the region.
 * This counts records from ALL boot cycles (past + current). */
int pstore_get_count(void);

/* Recover and print records from a previous boot cycle.
 * Called from pstore_init() when pre-existing data is found.
 * Also exposed so sysadmin tools can trigger a re-scan. */
void pstore_recover(void);

/* Panic/oops notifier callback (registered with notifier_chain_register).
 * Writes a final marker record and flushes any pending data. */
int pstore_panic_notifier(struct notifier_block *nb,
                          unsigned long action, void *data);

#endif /* PSTORE_H */
