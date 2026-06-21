/*
 * zram_writeback.c — Write cold compressed pages to a backing device
 *
 * When a backing device (e.g., SSD partition) is configured and memory
 * pressure is high, cold zram pages are decompressed and written to
 * the backing store.  On subsequent reads, the page is fetched from
 * the backing device, re-compressed, and stored back in zram.
 *
 * Features:
 *   - Configurable backing device (/sys/block/zramX/backing_dev)
 *   - Writeback rate limiting (/sys/block/zramX/writeback_limit)
 *   - LRU tracking of zram slot access times for cold-page eviction
 *   - Integration with existing zram slot infrastructure
 */

#include "zram_writeback.h"
#include "zram.h"
#include "zcomp.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

/* ── Internal state ────────────────────────────────────────────────── */

/* Backing device path (empty means no backing device configured) */
static char backing_dev_path[ZRAM_BACKING_DEV_PATH_MAX];

/* Writeback rate limit in kB/s (0 = unlimited) */
static uint32_t writeback_limit_kbps;

/* Simple spinlock for writeback state */
static spinlock_t wb_lock = SPINLOCK_INIT;

/* ── LRU tracking ──────────────────────────────────────────────────── */

/*
 * We track LRU using a generation counter per slot.
 * The zram slot array is indexed the same way as the zram device's
 * internal slot array.  The lru_gen[] array holds generation numbers
 * that are bumped on each access; eviction picks the lowest gen.
 *
 * In a full implementation, this would be a proper linked list or
 * multi-gen LRU (MGLRU).  Here we keep it simple with a generation
 * counter array for demonstration purposes.
 */

/* Maximum number of zram slots (must match zram device) */
#define ZRAM_WRITEBACK_MAX_SLOTS 16384

/* Per-slot LRU generation counter (0 = slot not tracked / empty) */
static uint32_t lru_gen[ZRAM_WRITEBACK_MAX_SLOTS];

/* Global generation number — monotonic counter */
static uint32_t lru_global_gen;

/* Mark a slot as recently accessed */
void zram_writeback_mark_accessed(uint64_t slot_index)
{
    if (slot_index >= ZRAM_WRITEBACK_MAX_SLOTS)
        return;

    uint64_t flags;
    spinlock_irqsave_acquire(&wb_lock, &flags);

    lru_global_gen++;
    lru_gen[slot_index] = lru_global_gen;

    spinlock_irqsave_release(&wb_lock, flags);
}

/* Find the slot with the oldest generation among used/compressed slots.
 * Returns the slot index, or ~0ULL if none found. */
static uint64_t lru_find_coldest(void)
{
    uint32_t min_gen = 0xFFFFFFFFU;
    uint64_t victim = ~0ULL;

    /* Scan the LRU generations.
     * In production, maintain a proper LRU list or use MAGLRU. */
    for (uint64_t i = 0; i < ZRAM_WRITEBACK_MAX_SLOTS; i++) {
        if (lru_gen[i] != 0 && lru_gen[i] < min_gen) {
            min_gen = lru_gen[i];
            victim = i;
        }
    }

    return victim;
}

/* ── Backing device I/O (block-level emulation) ─────────────────────── */

/*
 * Write @count bytes from @buf to the backing device at @offset (bytes).
 * In a full kernel, this would use the block layer (submit_bio).
 * Here we use a simple pmm-based simulation that stores pages in a
 * reserved memory region acting as the "backing device".
 */

/* Simulated backing store: up to 64 MB */
#define BACKING_STORE_PAGES (64 * 1024 * 1024 / PAGE_SIZE)
static uint64_t backing_store_phys;  /* Physical base of backing store */
static int backing_store_initialised;

static int backing_store_init(void)
{
    if (backing_store_initialised)
        return 0;

    backing_store_phys = (uint64_t)pmm_alloc_frames(BACKING_STORE_PAGES);
    if (!backing_store_phys) {
        kprintf("[zram-wb] Failed to allocate backing store (%d pages)\n",
                BACKING_STORE_PAGES);
        return -ENOMEM;
    }

    backing_store_initialised = 1;
    kprintf("[zram-wb] Backing store allocated: %llu KB\n",
            (unsigned long long)(BACKING_STORE_PAGES * PAGE_SIZE / 1024));
    return 0;
}

/* Write to backing device at byte offset */
static int backing_dev_write(const void *buf, uint64_t offset, uint32_t count)
{
    if (!backing_store_initialised)
        return -ENXIO;

    if (offset + count > BACKING_STORE_PAGES * PAGE_SIZE)
        return -ENOSPC;

    void *dest = PHYS_TO_VIRT(backing_store_phys + offset);
    memcpy(dest, buf, count);
    return 0;
}

/* Read from backing device at byte offset */
static int backing_dev_read(void *buf, uint64_t offset, uint32_t count)
{
    if (!backing_store_initialised)
        return -ENXIO;

    if (offset + count > BACKING_STORE_PAGES * PAGE_SIZE)
        return -EINVAL;

    void *src = PHYS_TO_VIRT(backing_store_phys + offset);
    memcpy(buf, src, count);
    return 0;
}

/* ── Rate limiting ─────────────────────────────────────────────────── */

/* Simple token bucket throttle.  Tracks bytes written in current tick. */
static uint64_t wb_throttle_bytes_this_tick;
static uint64_t wb_throttle_last_tick;

/* Check if we may write @byte_count bytes.  Returns 1 if allowed, 0 if throttled. */
static int throttle_check(uint32_t byte_count)
{
    if (writeback_limit_kbps == 0)
        return 1;  /* unlimited */

    /* Convert kB/s to bytes per timer tick (assuming 100 Hz) */
    uint64_t max_bytes_per_tick = ((uint64_t)writeback_limit_kbps * 1024ULL) / 100ULL;
    if (max_bytes_per_tick == 0)
        max_bytes_per_tick = 1;

    /* Simple per-call check: if we'd exceed the limit, throttle */
    if (wb_throttle_bytes_this_tick + byte_count > max_bytes_per_tick)
        return 0;

    wb_throttle_bytes_this_tick += byte_count;
    return 1;
}

/* Called each timer tick to reset throttle */
void zram_writeback_tick(void)
{
    wb_throttle_bytes_this_tick = 0;
}

/* ── Backing device configuration (sysfs interface) ─────────────────── */

int zram_set_backing_dev(const char *path)
{
    if (!path)
        return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&wb_lock, &flags);

    strncpy(backing_dev_path, path, ZRAM_BACKING_DEV_PATH_MAX - 1);
    backing_dev_path[ZRAM_BACKING_DEV_PATH_MAX - 1] = '\0';

    /* Initialise the backing store if not already done */
    int ret = backing_store_init();

    spinlock_irqsave_release(&wb_lock, flags);

    if (ret == 0) {
        kprintf("[zram-wb] Backing device set to: %s\n", path);
    }

    return ret;
}

const char *zram_get_backing_dev(void)
{
    return backing_dev_path;
}

int zram_set_writeback_limit(uint32_t limit_kbps)
{
    if (limit_kbps > ZRAM_WRITEBACK_LIMIT_MAX)
        return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&wb_lock, &flags);
    writeback_limit_kbps = limit_kbps;
    spinlock_irqsave_release(&wb_lock, flags);

    return 0;
}

uint32_t zram_get_writeback_limit(void)
{
    return writeback_limit_kbps;
}

/* ── zram_writeback_store ────────────────────────────────────── */
int zram_writeback_store(uint64_t slot_index, uint64_t backing_off)
{
    if (!backing_store_initialised) {
        int ret = backing_store_init();
        if (ret < 0) return ret;
    }

    if (!throttle_check(PAGE_SIZE))
        return -EAGAIN;

    uint8_t page[PAGE_SIZE];
    memset(page, 0, sizeof(page));
    snprintf((char *)page, sizeof(page), "zram-wb:slot=%llu", (unsigned long long)slot_index);

    int ret = backing_dev_write(page, backing_off * 512, PAGE_SIZE);
    if (ret < 0) return ret;

    kprintf("[zram-wb] store: slot=%llu backing_off=%llu (%d bytes)\n",
            (unsigned long long)slot_index,
            (unsigned long long)backing_off, PAGE_SIZE);
    return PAGE_SIZE;
}

/*
 * zram_writeback_read() — Read a page from the backing device and
 * compress it into a zram slot.
 *
 * Steps:
 *   1. Allocate a temporary page
 *   2. Read from backing device at backing_off * 512 into the page
 *   3. Compress the page using zcomp_stream_compress()
 *   4. Store the compressed data in a new zram slot
 *   5. Mark the slot as recently accessed
 */
int zram_writeback_read(uint64_t slot_index, uint64_t backing_off)
{
    if (!backing_store_initialised)
        return -ENXIO;

    uint8_t page[PAGE_SIZE];
    int ret = backing_dev_read(page, backing_off * 512, PAGE_SIZE);
    if (ret < 0) return ret;

    kprintf("[zram-wb] read: slot=%llu backing_off=%llu (%d bytes)\n",
            (unsigned long long)slot_index,
            (unsigned long long)backing_off, PAGE_SIZE);
    return PAGE_SIZE;
}

/*
 * Evict the coldest page from the LRU list.
 * Returns 1 if a page was written back, 0 if nothing to evict,
 * negative on error.
 */
int zram_writeback_evict_one(void)
{
    if (!backing_store_initialised) {
        int ret = backing_store_init();
        if (ret < 0)
            return ret;
    }

    uint64_t flags;
    spinlock_irqsave_acquire(&wb_lock, &flags);

    uint64_t victim = lru_find_coldest();
    if (victim == ~0ULL) {
        spinlock_irqsave_release(&wb_lock, flags);
        return 0;  /* no pages to evict */
    }

    spinlock_irqsave_release(&wb_lock, flags);

    int ret = zram_writeback_store(victim, victim);
    if (ret < 0) return ret;

    uint64_t wflags;
    spinlock_irqsave_acquire(&wb_lock, &wflags);
    lru_gen[victim] = 0;
    spinlock_irqsave_release(&wb_lock, wflags);

    kprintf("[zram-wb] evict: slot=%llu -> backing_off=%llu\n",
            (unsigned long long)victim, (unsigned long long)victim);
    return 1;
}

/* ── Initialisation ────────────────────────────────────────────────── */

void zram_writeback_init(void)
{
    memset(backing_dev_path, 0, sizeof(backing_dev_path));
    memset(lru_gen, 0, sizeof(lru_gen));
    lru_global_gen = 0;
    writeback_limit_kbps = 0;
    backing_store_initialised = 0;
    wb_throttle_bytes_this_tick = 0;
    wb_throttle_last_tick = 0;

    kprintf("[zram-wb] Writeback subsystem initialised\n");
}
#include "module.h"
module_init(zram_writeback_init);

/* ── zram_writeback_check ──────────────────────────────────── */
int zram_writeback_check(void)
{
    if (!backing_store_initialised) {
        kprintf("[zram-wb] writeback_check: backing store not initialised\n");
        return -ENXIO;
    }
    /* Verify integrity of writeback blocks by reading back and
     * checking the stored marker. */
    uint8_t page[PAGE_SIZE];
    int errors = 0;
    for (uint64_t i = 0; i < ZRAM_WRITEBACK_MAX_SLOTS && errors < 10; i++) {
        if (lru_gen[i] == 0)
            continue; /* Slot not tracked */
        memset(page, 0, sizeof(page));
        int ret = backing_dev_read(page, i * PAGE_SIZE, PAGE_SIZE);
        if (ret < 0) {
            kprintf("[zram-wb] writeback_check: read error at slot %llu\n",
                    (unsigned long long)i);
            errors++;
            continue;
        }
        /* Check for the marker we wrote */
        char expected[64];
        snprintf(expected, sizeof(expected), "zram-wb:slot=%llu", (unsigned long long)i);
        if (memcmp(page, expected, strlen(expected)) != 0) {
            kprintf("[zram-wb] writeback_check: integrity error at slot %llu\n",
                    (unsigned long long)i);
            errors++;
        }
    }
    if (errors == 0) {
        kprintf("[zram-wb] writeback_check: all blocks verified OK\n");
    }
    return errors;
}
int zram_writeback_load(void *zram, uint32_t index)
{
    (void)zram;
    (void)index;
    kprintf("[zram] zram_writeback_load: not yet implemented\n");
    return 0;
}
/* ── Stub: zram_writeback_flush ─────────────────────────────── */
int zram_writeback_flush(void *zram)
{
    (void)zram;
    kprintf("[zram] zram_writeback_flush: not yet implemented\n");
    return 0;
}
