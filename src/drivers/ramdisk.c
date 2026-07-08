#include "ramdisk.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "blockdev.h"

/* All pages for the ramdisk are allocated at init time.
 * Each page holds PAGE_SIZE / RAMDISK_SECTOR_SIZE sectors. */
#define SECTORS_PER_PAGE (PAGE_SIZE / RAMDISK_SECTOR_SIZE)
#define RAMDISK_PAGES    ((RAMDISK_SECTORS + SECTORS_PER_PAGE - 1) / SECTORS_PER_PAGE)

static uint8_t *ramdisk_pages[RAMDISK_PAGES];
static int ramdisk_ready = 0;
static int ramdisk_bdev_registered = 0;  /* tracks block-device registration */

static uint8_t *sector_ptr(uint32_t lba) {
    uint32_t page_idx = lba / SECTORS_PER_PAGE;
    uint32_t offset   = (lba % SECTORS_PER_PAGE) * RAMDISK_SECTOR_SIZE;
    return ramdisk_pages[page_idx] + offset;
}

void __init ramdisk_init(void) {
    if (ramdisk_ready) return;

    for (int i = 0; i < RAMDISK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            kprintf("[!!] ramdisk: OOM at page %d/%u\n", i, (unsigned)RAMDISK_PAGES);
            /* Free what we allocated so far */
            for (int j = 0; j < i; j++)
                pmm_free_frame((unsigned long)VIRT_TO_PHYS((unsigned long)ramdisk_pages[j]));
            return;
        }
        ramdisk_pages[i] = (uint8_t *)PHYS_TO_VIRT(frame);
        memset(ramdisk_pages[i], 0, PAGE_SIZE);
    }

    ramdisk_ready = 1;
    kprintf("[OK] Ramdisk: %u sectors (%u KB, %u pages)\n",
            (unsigned)RAMDISK_SECTORS,
            (unsigned)(RAMDISK_SECTORS * RAMDISK_SECTOR_SIZE / 1024),
            (unsigned)RAMDISK_PAGES);

    /* Register as a block device so filesystems can mount it */
    int ret = blockdev_register_legacy(BLOCKDEV_RAMDISK, "ram0",
                                        ramdisk_read_sectors,
                                        ramdisk_write_sectors,
                                        ramdisk_get_sectors);
    if (ret == 0) {
        ramdisk_bdev_registered = 1;
        kprintf("[OK] Ramdisk registered as block device ram0 (id=%d)\n",
                BLOCKDEV_RAMDISK);
    } else {
        kprintf("[!!] ramdisk: failed to register as block device (ret=%d)\n", ret);
    }
}

int ramdisk_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!ramdisk_ready) return -1;
    if (lba + count > RAMDISK_SECTORS) return -1;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(dst + i * RAMDISK_SECTOR_SIZE, sector_ptr(lba + i), RAMDISK_SECTOR_SIZE);
    }
    return 0;
}

int ramdisk_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!ramdisk_ready) return -1;
    if (lba + count > RAMDISK_SECTORS) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(sector_ptr(lba + i), src + i * RAMDISK_SECTOR_SIZE, RAMDISK_SECTOR_SIZE);
    }
    return 0;
}

int ramdisk_is_present(void) {
    return ramdisk_ready;
}

uint32_t ramdisk_get_sectors(void) {
    return RAMDISK_SECTORS;
}
#include "module.h"
module_init(ramdisk_init);

/* ── Byte-level read using sector-based backing store ─── */
static int ramdisk_read(void *buf, size_t count, uint64_t offset)
{
    if (!buf || !count || !ramdisk_ready)
        return -EINVAL;

    uint32_t start_lba = (uint32_t)(offset / RAMDISK_SECTOR_SIZE);
    uint32_t end_lba   = (uint32_t)((offset + count + RAMDISK_SECTOR_SIZE - 1) / RAMDISK_SECTOR_SIZE);
    uint32_t num_sectors = end_lba - start_lba;
    if (num_sectors == 0)
        return 0;

    /* Allocate temporary sector buffer */
    uint8_t tmp[RAMDISK_SECTOR_SIZE];

    size_t buf_off = 0;
    uint64_t remain = count;
    for (uint32_t i = 0; i < num_sectors && remain > 0; i++) {
        if (ramdisk_read_sectors(start_lba + i, 1, tmp) < 0)
            return -EIO;

        uint32_t copy_start = (i == 0) ? (uint32_t)(offset % RAMDISK_SECTOR_SIZE) : 0;
        uint32_t copy_len = (uint32_t)remain;
        if (copy_len > RAMDISK_SECTOR_SIZE - copy_start)
            copy_len = RAMDISK_SECTOR_SIZE - copy_start;

        memcpy((uint8_t *)buf + buf_off, tmp + copy_start, copy_len);
        buf_off += copy_len;
        remain -= copy_len;
    }

    return 0;
}

/* ── Byte-level write using sector-based backing store ─── */
static int ramdisk_write(const void *buf, size_t count, uint64_t offset)
{
    if (!buf || !count || !ramdisk_ready)
        return -EINVAL;

    uint32_t start_lba = (uint32_t)(offset / RAMDISK_SECTOR_SIZE);
    uint32_t end_lba   = (uint32_t)((offset + count + RAMDISK_SECTOR_SIZE - 1) / RAMDISK_SECTOR_SIZE);
    uint32_t num_sectors = end_lba - start_lba;
    if (num_sectors == 0)
        return 0;

    uint8_t tmp[RAMDISK_SECTOR_SIZE];

    size_t buf_off = 0;
    uint64_t remain = count;
    for (uint32_t i = 0; i < num_sectors && remain > 0; i++) {
        uint32_t copy_start = (i == 0) ? (uint32_t)(offset % RAMDISK_SECTOR_SIZE) : 0;
        uint32_t copy_len = (uint32_t)remain;
        if (copy_len > RAMDISK_SECTOR_SIZE - copy_start)
            copy_len = RAMDISK_SECTOR_SIZE - copy_start;

        /* Read-modify-write if not covering the whole sector */
        if (copy_start != 0 || copy_len != RAMDISK_SECTOR_SIZE) {
            if (ramdisk_read_sectors(start_lba + i, 1, tmp) < 0)
                return -EIO;
        }

        memcpy(tmp + copy_start, (const uint8_t *)buf + buf_off, copy_len);
        if (ramdisk_write_sectors(start_lba + i, 1, tmp) < 0)
            return -EIO;

        buf_off += copy_len;
        remain -= copy_len;
    }

    return 0;
}

/* ── ioctl stub (ramdisk does not support any ioctls) ─── */
static int ramdisk_ioctl(int cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    return -ENOTTY;
}
