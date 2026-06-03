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

void ramdisk_init(void) {
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
