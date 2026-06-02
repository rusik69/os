/*
 * zram.c — Compressed RAM block device with multi-stream compression
 *
 * Supports multiple compression algorithms and per-CPU streams
 * for lock-free concurrent compression/decompression.
 *
 * Features:
 *   - Per-CPU compression streams (no lock contention on SMP)
 *   - Multiple algorithm selection (fast, lzss, none)
 *   - Transparent compress-on-write, decompress-on-read
 *   - Detailed compression statistics
 */

#include "zram.h"
#include "zcomp.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "smp.h"
#include "heap.h"

/* ── ZRAM slot: points to compressed data in memory ────────────────── */

struct zram_slot {
    uint64_t comp_addr;  /* Physical address of compressed data (0 = empty) */
    uint32_t comp_len;   /* Compressed length in bytes (0 = empty) */
    uint32_t orig_len;   /* Original length in bytes (must be PAGE_SIZE) */
    uint8_t  algo_id;    /* Algorithm used to compress this slot */
    uint8_t  flags;      /* ZRAM_SLOT_FLAG_* */
};

/* Slot flags */
#define ZRAM_SLOT_FLAG_INCOMPRESSIBLE  0x01  /* Stored raw (no compression) */

/* ── ZRAM device ───────────────────────────────────────────────────── */

struct zram_device {
    uint64_t disk_size;         /* Total size in bytes */
    uint64_t num_slots;         /* Number of 4K slots */
    struct zram_slot *slots;    /* Slot metadata array */

    /* Compression state */
    uint32_t algo_id;                       /* Current algorithm ID */
    const struct zcomp_ops *ops;            /* Current algorithm ops */
    struct zcomp_stream streams[ZRAM_MAX_STREAMS]; /* Per-CPU streams */
    int num_streams;                        /* Number of configured streams */

    /* Statistics */
    uint64_t comp_total;        /* Total compressed bytes stored */
    uint64_t orig_total;        /* Total original bytes stored */
    uint64_t stored_pages;      /* Number of pages stored */
    uint64_t incompressible;    /* Count of incompressible pages */

    int initialized;
};

static struct zram_device zram_dev;

/* ── Initialization ────────────────────────────────────────────────── */

void zram_init(void)
{
    memset(&zram_dev, 0, sizeof(zram_dev));
    zram_dev.algo_id = ZCOMP_ALGO_FAST;
    zram_dev.num_streams = 0;
    kprintf("[mem] ZRAM subsystem initialized\n");
}

/* ── Device creation ───────────────────────────────────────────────── */

int zram_create_device_default(uint64_t disk_size)
{
    return zram_create_device(disk_size, ZCOMP_ALGO_FAST);
}

int zram_create_device(uint64_t disk_size, uint32_t algo_id)
{
    const struct zcomp_ops *ops;
    int slot_pages, ret;
    uint64_t slot_phys;
    size_t slot_array_size;

    if (zram_dev.initialized)
        return -EBUSY;
    if (disk_size == 0 || disk_size % PAGE_SIZE != 0)
        return -EINVAL;

    /* Find the requested algorithm */
    ops = zcomp_find(algo_id);
    if (!ops) {
        kprintf("[zram] Unknown compression algorithm %u, using default\n",
                (unsigned)algo_id);
        ops = zcomp_find(ZCOMP_ALGO_FAST);
        if (!ops)
            return -ENODEV;
        algo_id = ZCOMP_ALGO_FAST;
    }

    /* Initialize per-CPU compression streams */
    zram_dev.num_streams = smp_get_cpu_count();
    if (zram_dev.num_streams < 1)
        zram_dev.num_streams = 1;
    if (zram_dev.num_streams > ZRAM_MAX_STREAMS)
        zram_dev.num_streams = ZRAM_MAX_STREAMS;

    ret = zcomp_streams_init(zram_dev.streams, zram_dev.num_streams, ops);
    if (ret < 0) {
        kprintf("[zram] Failed to init compression streams: %d\n", ret);
        return ret;
    }

    /* Allocate slot metadata */
    zram_dev.disk_size = disk_size;
    zram_dev.num_slots = disk_size / PAGE_SIZE;

    slot_array_size = zram_dev.num_slots * sizeof(struct zram_slot);
    slot_pages = (int)((slot_array_size + PAGE_SIZE - 1) / PAGE_SIZE);
    slot_phys = (uint64_t)pmm_alloc_frames((uint64_t)slot_pages);
    if (!slot_phys) {
        zcomp_streams_destroy(zram_dev.streams, zram_dev.num_streams);
        zram_dev.num_streams = 0;
        return -ENOMEM;
    }

    zram_dev.slots = (struct zram_slot *)PHYS_TO_VIRT(slot_phys);
    memset(zram_dev.slots, 0, slot_array_size);
    zram_dev.ops = ops;
    zram_dev.algo_id = algo_id;
    zram_dev.initialized = 1;

    kprintf("[zram] Device: %llu MB, %llu slots, algo=%s, %d streams\n",
            (unsigned long long)(disk_size / (1024 * 1024)),
            (unsigned long long)zram_dev.num_slots,
            ops->name,
            zram_dev.num_streams);
    return 0;
}

/* ── Read sectors ──────────────────────────────────────────────────── */

int zram_read_sectors(uint64_t sector, void *buf, uint32_t count)
{
    struct zcomp_stream *zs;
    uint8_t *buf_bytes = (uint8_t *)buf;

    if (!zram_dev.initialized)
        return -ENXIO;
    if (!buf)
        return -EINVAL;
    if (sector + count > zram_dev.num_slots)
        return -ENXIO;

    /* Get per-CPU stream for current CPU */
    zs = zcomp_stream_get(zram_dev.streams, zram_dev.num_streams);
    if (!zs || !zs->ops)
        return -EINVAL;

    for (uint32_t i = 0; i < count; i++) {
        struct zram_slot *slot = &zram_dev.slots[sector + i];

        if (slot->comp_len == 0) {
            /* Slot is empty — return zeros */
            memset(buf_bytes + i * PAGE_SIZE, 0, PAGE_SIZE);
        } else if (slot->flags & ZRAM_SLOT_FLAG_INCOMPRESSIBLE) {
            /* Stored raw — direct copy */
            void *raw_virt = PHYS_TO_VIRT(slot->comp_addr);
            memcpy(buf_bytes + i * PAGE_SIZE, raw_virt, PAGE_SIZE);
        } else {
            /* Decompress into buffer */
            void *comp_virt = PHYS_TO_VIRT(slot->comp_addr);
            int ret = zcomp_stream_decompress(zs,
                          (const uint8_t *)comp_virt, slot->comp_len,
                          buf_bytes + i * PAGE_SIZE, PAGE_SIZE);
            if (ret < 0) {
                kprintf("[zram] Decompression error at sector %llu: %d\n",
                        (unsigned long long)(sector + i), ret);
                memset(buf_bytes + i * PAGE_SIZE, 0, PAGE_SIZE);
            }
        }
    }
    return 0;
}

/* ── Write sectors ─────────────────────────────────────────────────── */

int zram_write_sectors(uint64_t sector, const void *buf, uint32_t count)
{
    struct zcomp_stream *zs;
    const uint8_t *buf_bytes = (const uint8_t *)buf;

    if (!zram_dev.initialized)
        return -ENXIO;
    if (!buf)
        return -EINVAL;
    if (sector + count > zram_dev.num_slots)
        return -ENXIO;

    /* Get per-CPU stream for current CPU */
    zs = zcomp_stream_get(zram_dev.streams, zram_dev.num_streams);
    if (!zs || !zs->ops)
        return -EINVAL;

    /* Allocate a page for compressed data per sector */
    uint8_t *comp_buf = (uint8_t *)kmalloc(PAGE_SIZE * 2);
    if (!comp_buf)
        return -ENOMEM;

    for (uint32_t i = 0; i < count; i++) {
        struct zram_slot *slot = &zram_dev.slots[sector + i];

        /* Free existing compressed page if any */
        if (slot->comp_addr) {
            pmm_free_frame(slot->comp_addr);
            slot->comp_addr = 0;
            slot->comp_len = 0;
        }

        /* Try to compress the page */
        int comp_len = zcomp_stream_compress(zs,
                          buf_bytes + i * PAGE_SIZE, PAGE_SIZE,
                          comp_buf, PAGE_SIZE * 2);

        if (comp_len > 0 && (size_t)comp_len < PAGE_SIZE) {
            /* Compression successful and beneficial */
            uint64_t comp_page = pmm_alloc_frame();
            if (!comp_page) {
                kfree(comp_buf);
                return -ENOMEM;
            }

            void *comp_virt = PHYS_TO_VIRT(comp_page);
            memcpy(comp_virt, comp_buf, (size_t)comp_len);

            slot->comp_addr = comp_page;
            slot->comp_len = (uint32_t)comp_len;
            slot->orig_len = PAGE_SIZE;
            slot->algo_id = (uint8_t)zram_dev.algo_id;
            slot->flags = 0;

            zram_dev.comp_total += (uint64_t)comp_len;
            zram_dev.stored_pages++;
        } else {
            /* Data is incompressible or compression failed — store raw.
             * We still try to save memory by storing the raw page, but
             * mark it as incompressible so we don't attempt decompression. */
            uint64_t raw_page = pmm_alloc_frame();
            if (!raw_page) {
                kfree(comp_buf);
                return -ENOMEM;
            }

            void *raw_virt = PHYS_TO_VIRT(raw_page);
            memcpy(raw_virt, buf_bytes + i * PAGE_SIZE, PAGE_SIZE);

            slot->comp_addr = raw_page;
            slot->comp_len = PAGE_SIZE;
            slot->orig_len = PAGE_SIZE;
            slot->algo_id = (uint8_t)zram_dev.algo_id;
            slot->flags = ZRAM_SLOT_FLAG_INCOMPRESSIBLE;

            zram_dev.comp_total += PAGE_SIZE;
            zram_dev.stored_pages++;
            zram_dev.incompressible++;

            if (comp_len == 0) {
                /* Algorithm said "incompressible" — normal */
            } else {
                /* Algorithm error — still store raw as fallback */
                kprintf("[zram] Compression failed for sector %llu: %d, storing raw\n",
                        (unsigned long long)(sector + i), comp_len);
            }
        }

        zram_dev.orig_total += PAGE_SIZE;
    }

    kfree(comp_buf);
    return 0;
}

/* ── Statistics ────────────────────────────────────────────────────── */

uint64_t zram_get_compressed_size(void)   { return zram_dev.comp_total; }
uint64_t zram_get_orig_size(void)         { return zram_dev.orig_total; }
uint64_t zram_get_stored_pages(void)      { return zram_dev.stored_pages; }

/* ── Algorithm management ──────────────────────────────────────────── */

int zram_set_algorithm(uint32_t algo_id)
{
    const struct zcomp_ops *ops;

    if (zram_dev.initialized)
        return -EBUSY; /* Cannot change while device is in use */

    ops = zcomp_find(algo_id);
    if (!ops)
        return -EINVAL;

    zram_dev.algo_id = algo_id;
    zram_dev.ops = ops;
    return 0;
}

uint32_t zram_get_algorithm(void)
{
    return zram_dev.algo_id;
}
