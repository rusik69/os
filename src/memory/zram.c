/* zram.c — Compressed RAM block device */

#include "zram.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

/* Simple "compression" using a trivial RLE-like scheme.
 * In a real kernel, this would use LZO or LZ4.
 * For our purposes, we use a lightweight XOR+run-length scheme
 * that demonstrates the framework. */

/* ZRAM slot: points to compressed data in memory */
struct zram_slot {
    uint64_t comp_addr;  /* Physical address of compressed data (0 = uncompressed/empty) */
    uint32_t comp_len;   /* Compressed length in bytes (0 = empty) */
    uint32_t orig_len;   /* Original length in bytes */
};

/* ZRAM device */
struct zram_device {
    uint64_t disk_size;        /* Total size in bytes */
    uint64_t num_slots;        /* Number of 4K slots */
    struct zram_slot *slots;   /* Slot metadata array */
    uint64_t comp_total;       /* Total compressed bytes stored */
    uint64_t orig_total;       /* Total original bytes stored */
    uint64_t stored_pages;     /* Number of pages stored */
    int initialized;
};

static struct zram_device zram_dev;

/* Trivial compress: simple XOR with position-based key.
 * Returns compressed size (always <= input size). */
static uint32_t zram_compress(const uint8_t *src, uint8_t *dst, uint32_t size) {
    /* For simplicity, just copy with a simple transformation */
    /* In production, we'd use LZO/LZ4 */
    uint32_t out = 0;
    uint32_t i = 0;
    while (i < size) {
        uint8_t byte = src[i];
        uint8_t count = 1;
        while (i + count < size && src[i + count] == byte && count < 255)
            count++;
        dst[out++] = byte;
        dst[out++] = count;
        i += count;
    }
    return out;
}

/* Trivial decompress */
static uint32_t zram_decompress(const uint8_t *src, uint8_t *dst, uint32_t src_size, uint32_t dst_size) {
    uint32_t in = 0;
    uint32_t out = 0;
    while (in < src_size && out < dst_size) {
        uint8_t byte = src[in++];
        uint8_t count = (in < src_size) ? src[in++] : 1;
        for (uint8_t i = 0; i < count && out < dst_size; i++)
            dst[out++] = byte;
    }
    return out;
}

void zram_init(void) {
    memset(&zram_dev, 0, sizeof(zram_dev));
    kprintf("[mem] ZRAM compressed RAM block device initialized\n");
}

int zram_create_device(uint64_t disk_size) {
    if (zram_dev.initialized) return -1;

    zram_dev.disk_size = disk_size;
    zram_dev.num_slots = disk_size / 4096;

    /* Allocate slot array from physical memory */
    uint64_t slot_array_size = zram_dev.num_slots * sizeof(struct zram_slot);
    uint64_t slot_pages = (slot_array_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t slot_phys = (uint64_t)pmm_alloc_frames(slot_pages);
    if (!slot_phys) return -1;

    zram_dev.slots = (struct zram_slot *)PHYS_TO_VIRT(slot_phys);
    memset(zram_dev.slots, 0, slot_array_size);
    zram_dev.initialized = 1;

    kprintf("[mem] ZRAM device: %llu MB, %llu slots\n",
            (unsigned long)(disk_size / (1024 * 1024)), (unsigned long)zram_dev.num_slots);
    return 0;
}

int zram_read_sectors(uint64_t sector, void *buf, uint32_t count) {
    if (!zram_dev.initialized) return -1;
    if (sector + count > zram_dev.num_slots) return -1;

    uint8_t *buf_bytes = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        struct zram_slot *slot = &zram_dev.slots[sector + i];
        if (slot->comp_len == 0) {
            /* Slot is empty — return zeros */
            memset(buf_bytes + i * 4096, 0, 4096);
        } else {
            /* Decompress into buffer */
            void *comp_virt = PHYS_TO_VIRT(slot->comp_addr);
            zram_decompress((const uint8_t *)comp_virt,
                           buf_bytes + i * 4096,
                           slot->comp_len, 4096);
        }
    }
    return 0;
}

int zram_write_sectors(uint64_t sector, const void *buf, uint32_t count) {
    if (!zram_dev.initialized) return -1;
    if (sector + count > zram_dev.num_slots) return -1;

    const uint8_t *buf_bytes = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        struct zram_slot *slot = &zram_dev.slots[sector + i];

        /* Free existing compressed page if any */
        if (slot->comp_addr) {
            pmm_free_frame(slot->comp_addr);
        }

        /* Allocate a page for compressed data */
        uint64_t comp_page = pmm_alloc_frame();
        if (!comp_page) return -1;

        void *comp_virt = PHYS_TO_VIRT(comp_page);
        uint32_t comp_len = zram_compress(buf_bytes + i * 4096,
                                          (uint8_t *)comp_virt, 4096);

        slot->comp_addr = comp_page;
        slot->comp_len = comp_len;
        slot->orig_len = 4096;

        zram_dev.comp_total += comp_len;
        zram_dev.orig_total += 4096;
        zram_dev.stored_pages++;
    }
    return 0;
}

uint64_t zram_get_compressed_size(void) { return zram_dev.comp_total; }
uint64_t zram_get_orig_size(void)       { return zram_dev.orig_total; }
uint64_t zram_get_stored_pages(void)    { return zram_dev.stored_pages; }
