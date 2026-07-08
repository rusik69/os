/*
 * swap.c — Block device swap subsystem (Item 223)
 *
 * Implements swapon/swapoff for block devices, providing page-level
 * swap-out/swap-in.  Each swap area has a superblock (page 0) with
 * magic + metadata, followed by a slot bitmap and data pages.
 *
 * Block device I/O uses blk_submit_sync() at sector granularity.
 * Each swap slot is one 4K page = 8 sectors (assuming 512B sectors).
 *
 * Usage:
 *   1. Prepare a swap area on a block device (e.g., with mkswap).
 *   2. Call swapon("sda2") or swapon("/dev/sda2") to activate it.
 *   3. The kernel calls swap_out/swap_in to move pages.
 */

#define KERNEL_INTERNAL
#include "swap.h"
#include "blockdev.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "export.h"
#include "zswap.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define SWAP_SECTOR_SIZE      512                     /* bytes per sector          */
#define SWAP_PAGES_PER_SLOT   1                       /* pages per swap slot       */
#define SWAP_SECTORS_PER_PAGE (SWAP_SLOT_SIZE / SWAP_SECTOR_SIZE)  /* 8        */

/* ── Global state ──────────────────────────────────────────────────── */

static struct swap_device swap_devices[SWAP_MAX_DEVICES];
static int                swap_initialised = 0;
static spinlock_t         swap_global_lock = SPINLOCK_INIT;

/* ── Checksum ──────────────────────────────────────────────────────── */

/* Simple XOR checksum over the superblock (excluding the checksum field). */
static uint32_t swap_checksum(const struct swap_superblock *sb)
{
    const uint8_t *bytes = (const uint8_t *)sb;
    uint32_t xor_sum = 0;
    /* checksum is at offset 24, size 4 bytes — skip it */
    for (size_t i = 0; i < 24; i++)
        xor_sum ^= (uint32_t)bytes[i] << (i % 4) * 8;
    for (size_t i = 28; i < sizeof(struct swap_superblock); i++)
        xor_sum ^= (uint32_t)bytes[i] << (i % 4) * 8;
    return xor_sum;
}

/* ── Block device I/O helpers ──────────────────────────────────────── */

/* Read one swap page (SWAP_SLOT_SIZE = 4096 bytes) from a device.
 * Converts byte offset to LBA (sector) and calls blk_submit_sync. */
static int swap_read_slot(int dev_id, uint64_t byte_offset, void *buf)
{
    uint64_t lba = byte_offset / SWAP_SECTOR_SIZE;
    uint32_t count = SWAP_SECTORS_PER_PAGE;  /* 8 sectors = 4K */
    return blk_submit_sync(dev_id, lba, count, buf, BLK_REQ_READ);
}

/* Write one swap page to a device. */
static int swap_write_slot(int dev_id, uint64_t byte_offset, const void *buf)
{
    uint64_t lba = byte_offset / SWAP_SECTOR_SIZE;
    uint32_t count = SWAP_SECTORS_PER_PAGE;  /* 8 sectors = 4K */
    return blk_submit_sync(dev_id, lba, count, (void *)(uintptr_t)buf, BLK_REQ_WRITE);
}

/* ── Initialisation ────────────────────────────────────────────────── */

void __init swap_init(void)
{
    if (swap_initialised) return;

    memset(swap_devices, 0, sizeof(swap_devices));
    spinlock_init(&swap_global_lock);
    swap_initialised = 1;

    kprintf("[SWAP] Swap subsystem initialized (%d max devices)\n",
            SWAP_MAX_DEVICES);
}

/* ── Internal: find a free device slot ─────────────────────────────── */

static int swap_find_device_slot(void)
{
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        if (!swap_devices[i].active && !swap_devices[i].path[0])
            return i;
    }
    return -1;
}

/* ── Internal: find an active device for allocation ────────────────── */

static int swap_pick_active_device(void)
{
    /* Simple policy: use the first active device with free slots. */
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        if (swap_devices[i].active &&
            swap_devices[i].used_slots < swap_devices[i].total_slots)
            return i;
    }
    return -1;
}

/* ── Internal: bitmap operations ───────────────────────────────────── */

/* Find a zero bit in the bitmap.  Returns -1 if all bits are set. */
static int swap_bitmap_find_zero(const uint64_t *bitmap, uint32_t n_words)
{
    for (uint32_t w = 0; w < n_words; w++) {
        if (bitmap[w] != ~0ULL) {
            uint64_t inverted = ~bitmap[w];
            int bit = __builtin_ctzll(inverted);
            if (bit + (int)(w * 64) < 0) continue;
            return (int)(w * 64 + bit);
        }
    }
    return -1;
}

/* Set a bit in the bitmap (mark slot as used). */
static void swap_bitmap_set(uint64_t *bitmap, int slot)
{
    bitmap[slot / 64] |= (1ULL << (slot % 64));
}

/* Clear a bit in the bitmap (mark slot as free). */
static void swap_bitmap_clear(uint64_t *bitmap, int slot)
{
    bitmap[slot / 64] &= ~(1ULL << (slot % 64));
}

/* ── swapon ────────────────────────────────────────────────────────── */

int swap_swapon(const char *name)
{
    if (!swap_initialised) return -ENODEV;
    if (!name || !name[0]) return -EINVAL;

    /* Strip "/dev/" prefix if present and resolve to block device ID. */
    const char *devname = name;
    if (strncmp(name, "/dev/", 5) == 0)
        devname = name + 5;
    if (!devname[0]) return -EINVAL;

    int dev_id = blockdev_find_by_name(devname);
    if (dev_id < 0) {
        kprintf("[SWAP] swapon: device '%s' not found\n", devname);
        return -ENODEV;
    }

    spinlock_acquire(&swap_global_lock);

    /* Check if this device is already an active swap area. */
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        if (swap_devices[i].active &&
            swap_devices[i].blockdev_id == dev_id) {
            spinlock_release(&swap_global_lock);
            return -EBUSY;
        }
    }

    /* Find a free swap device slot. */
    int idx = swap_find_device_slot();
    if (idx < 0) {
        spinlock_release(&swap_global_lock);
        return -ENFILE;
    }

    struct swap_device *dev = &swap_devices[idx];

    /* Read the superblock (page 0, byte offset 0). */
    struct swap_superblock sb;
    int ret = swap_read_slot(dev_id, 0, &sb);
    if (ret < 0) {
        kprintf("[SWAP] swapon: cannot read superblock from '%s' (err=%d)\n",
                devname, ret);
        spinlock_release(&swap_global_lock);
        return ret;
    }

    /* Validate the superblock. */
    if (sb.magic != SWAP_MAGIC) {
        kprintf("[SWAP] swapon: '%s' has invalid magic (got 0x%016llx, "
                "expected 0x%016llx)\n", devname,
                (unsigned long long)sb.magic,
                (unsigned long long)SWAP_MAGIC);
        spinlock_release(&swap_global_lock);
        return -EINVAL;
    }
    if (sb.version != SWAP_VERSION) {
        kprintf("[SWAP] swapon: '%s' unsupported version %u\n",
                devname, sb.version);
        spinlock_release(&swap_global_lock);
        return -EINVAL;
    }

    /* Verify checksum. */
    uint32_t expected_csum = swap_checksum(&sb);
    if (sb.checksum != expected_csum) {
        kprintf("[SWAP] swapon: '%s' bad checksum (got 0x%08x, expected 0x%08x)\n",
                devname, sb.checksum, expected_csum);
        spinlock_release(&swap_global_lock);
        return -EILSEQ;
    }

    if (sb.total_slots == 0) {
        kprintf("[SWAP] swapon: '%s' has zero data slots\n", devname);
        spinlock_release(&swap_global_lock);
        return -ENODEV;
    }

    /* Allocate and read the slot bitmap. */
    uint32_t n_words = SWAP_BITMAP_SIZE(sb.total_slots);
    uint64_t *bitmap = (uint64_t *)kmalloc_array(n_words, sizeof(uint64_t));
    if (!bitmap) {
        spinlock_release(&swap_global_lock);
        return -ENOMEM;
    }
    memset(bitmap, 0, n_words * sizeof(uint64_t));

    /* Bitmap starts after the superblock page. */
    uint64_t bitmap_offset = SWAP_SUPERBLOCK_PAGES * SWAP_SLOT_SIZE;
    for (uint32_t w = 0; w < n_words; w++) {
        ret = swap_read_slot(dev_id, bitmap_offset + w * sizeof(uint64_t),
                             &bitmap[w]);
        if (ret < 0) {
            kprintf("[SWAP] swapon: failed to read bitmap word %u (err=%d)\n",
                    w, ret);
            kfree(bitmap);
            spinlock_release(&swap_global_lock);
            return ret;
        }
    }

    /* Fill in the device descriptor. */
    strncpy(dev->path, blockdev_name(dev_id), SWAP_NAME_MAX - 1);
    dev->path[SWAP_NAME_MAX - 1] = '\0';
    dev->blockdev_id  = dev_id;
    dev->total_slots  = sb.total_slots;
    dev->used_slots   = sb.used_slots;
    dev->bitmap       = bitmap;
    dev->bitmap_slots = n_words;
    dev->flags        = sb.flags;
    dev->active       = 1;

    kprintf("[SWAP] swapon: '%s' (id=%d) — %u total slots, %u used\n",
            dev->path, dev_id, sb.total_slots, sb.used_slots);

    spinlock_release(&swap_global_lock);
    return 0;
}

/* ── swapoff ───────────────────────────────────────────────────────── */

int swap_swapoff(const char *name)
{
    if (!swap_initialised) return -ENODEV;
    if (!name || !name[0]) return -EINVAL;

    const char *devname = name;
    if (strncmp(name, "/dev/", 5) == 0)
        devname = name + 5;
    if (!devname[0]) return -EINVAL;

    int dev_id = blockdev_find_by_name(devname);
    if (dev_id < 0) return -ENXIO;

    spinlock_acquire(&swap_global_lock);

    /* Find the swap device by blockdev ID. */
    int idx = -1;
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        if (swap_devices[i].active &&
            swap_devices[i].blockdev_id == dev_id) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spinlock_release(&swap_global_lock);
        return -ENXIO;
    }

    struct swap_device *dev = &swap_devices[idx];

    /* Refuse to deactivate if slots are still in use. */
    if (dev->used_slots > 0) {
        kprintf("[SWAP] swapoff: '%s' has %u slots still in use\n",
                devname, dev->used_slots);
        spinlock_release(&swap_global_lock);
        return -EBUSY;
    }

    /* Write the updated bitmap back to the device. */
    uint64_t bitmap_offset = SWAP_SUPERBLOCK_PAGES * SWAP_SLOT_SIZE;
    for (uint32_t w = 0; w < dev->bitmap_slots; w++) {
        int ret = swap_write_slot(dev_id, bitmap_offset + w * sizeof(uint64_t),
                                  &dev->bitmap[w]);
        if (ret < 0) {
            kprintf("[SWAP] swapoff: warning — failed to write bitmap word %u"
                    " (err=%d)\n", w, ret);
        }
    }

    /* Update the superblock on disk to mark the swap area as disabled. */
    {
        struct swap_superblock sb;
        int ret = swap_read_slot(dev_id, 0, &sb);
        if (ret == 0) {
            sb.flags &= ~SWAP_FLAG_ENABLED;
            sb.checksum = swap_checksum(&sb);
            swap_write_slot(dev_id, 0, &sb);
        }
    }

    /* Free resources. */
    if (dev->bitmap) kfree(dev->bitmap);
    memset(dev, 0, sizeof(*dev));

    kprintf("[SWAP] swapoff: '%s' deactivated\n", devname);

    spinlock_release(&swap_global_lock);
    return 0;
}

/* ── swap_out ──────────────────────────────────────────────────────── */

int swap_out(uint64_t phys_addr, uint32_t *out_slot, int *out_dev)
{
    if (!swap_initialised) return -ENODEV;
    if (!out_slot || !out_dev) return -EINVAL;

    spinlock_acquire(&swap_global_lock);

    int dev_idx = swap_pick_active_device();
    if (dev_idx < 0) {
        spinlock_release(&swap_global_lock);
        return -ENOSPC;
    }

    struct swap_device *dev = &swap_devices[dev_idx];

    /* Find a free slot in the bitmap. */
    int slot = swap_bitmap_find_zero(dev->bitmap, dev->bitmap_slots);
    if (slot < 0 || (uint32_t)slot >= dev->total_slots) {
        spinlock_release(&swap_global_lock);
        return -ENOSPC;
    }

    /* Calculate byte offset of the slot data on the device.
     * Layout: [superblock][bitmap][slot_0][slot_1]... */
    size_t   bitmap_bytes = dev->bitmap_slots * sizeof(uint64_t);
    uint64_t data_start   = (SWAP_SUPERBLOCK_PAGES * SWAP_SLOT_SIZE) + bitmap_bytes;
    uint64_t slot_offset  = data_start + (uint64_t)slot * SWAP_SLOT_SIZE;

    /* Read the physical page contents and write to the swap device.
     * phys_addr is a physical address — convert to kernel virtual. */
    void *page_data = (void *)(phys_addr + 0xFFFF800000000000ULL);
    int ret = swap_write_slot(dev->blockdev_id, slot_offset, page_data);
    if (ret < 0) {
        kprintf("[SWAP] swap_out: write failed (dev=%d, slot=%d, err=%d)\n",
                dev_idx, slot, ret);
        spinlock_release(&swap_global_lock);
        return ret;
    }

    /* Try to store a compressed copy in zswap for faster swap-in.
     * This is best-effort: if zswap is full or compression fails,
     * the data is safely on disk already. */
    zswap_store(phys_addr, dev_idx, (uint32_t)slot);

    /* Mark the slot as used. */
    swap_bitmap_set(dev->bitmap, slot);
    dev->used_slots++;

    /* Update the superblock's used_slots count on disk. */
    {
        struct swap_superblock sb;
        if (swap_read_slot(dev->blockdev_id, 0, &sb) == 0) {
            sb.used_slots = dev->used_slots;
            sb.checksum = swap_checksum(&sb);
            swap_write_slot(dev->blockdev_id, 0, &sb);
        }
    }

    *out_slot = (uint32_t)slot;
    *out_dev  = dev_idx;

    spinlock_release(&swap_global_lock);
    return 0;
}

/* ── swap_in ───────────────────────────────────────────────────────── */

int swap_in(int dev_idx, uint32_t slot, uint64_t phys_addr)
{
    if (!swap_initialised) return -ENODEV;
    if (dev_idx < 0 || dev_idx >= SWAP_MAX_DEVICES) return -EINVAL;

    spinlock_acquire(&swap_global_lock);

    struct swap_device *dev = &swap_devices[dev_idx];
    if (!dev->active) {
        spinlock_release(&swap_global_lock);
        return -ENODEV;
    }
    if (slot >= dev->total_slots) {
        spinlock_release(&swap_global_lock);
        return -EINVAL;
    }

    /* Calculate byte offset of the slot. */
    size_t   bitmap_bytes = dev->bitmap_slots * sizeof(uint64_t);
    uint64_t data_start   = (SWAP_SUPERBLOCK_PAGES * SWAP_SLOT_SIZE) + bitmap_bytes;
    uint64_t slot_offset  = data_start + (uint64_t)slot * SWAP_SLOT_SIZE;

    /* Try zswap first — if the page is in the compressed cache,
     * decompress and skip the disk read entirely. */
    void *page_data = (void *)(phys_addr + 0xFFFF800000000000ULL);
    if (zswap_load(dev_idx, slot, phys_addr) == 0) {
        spinlock_release(&swap_global_lock);
        return 0;
    }

    /* Read the slot data into the physical page from disk. */
    int ret = swap_read_slot(dev->blockdev_id, slot_offset, page_data);
    if (ret < 0) {
        kprintf("[SWAP] swap_in: read failed (dev=%d, slot=%u, err=%d)\n",
                dev_idx, slot, ret);
    }

    spinlock_release(&swap_global_lock);
    return ret;
}

/* ── swap_free_slot ────────────────────────────────────────────────── */

int swap_free_slot(int dev_idx, uint32_t slot)
{
    if (!swap_initialised) return -ENODEV;
    if (dev_idx < 0 || dev_idx >= SWAP_MAX_DEVICES) return -EINVAL;

    spinlock_acquire(&swap_global_lock);

    struct swap_device *dev = &swap_devices[dev_idx];
    if (!dev->active) {
        spinlock_release(&swap_global_lock);
        return -ENODEV;
    }
    if (slot >= dev->total_slots) {
        spinlock_release(&swap_global_lock);
        return -EINVAL;
    }

    /* Verify the slot was actually used (warn on double-free). */
    if (!(dev->bitmap[slot / 64] & (1ULL << (slot % 64)))) {
        kprintf("[SWAP] swap_free_slot: slot %u on dev %d was already free\n",
                slot, dev_idx);
        spinlock_release(&swap_global_lock);
        return 0;
    }

    swap_bitmap_clear(dev->bitmap, slot);
    dev->used_slots--;

    /* Also free the slot from zswap if it was cached there. */
    zswap_free(dev_idx, slot);

    /* Update the superblock's used_slots count on disk. */
    {
        struct swap_superblock sb;
        if (swap_read_slot(dev->blockdev_id, 0, &sb) == 0) {
            sb.used_slots = dev->used_slots;
            sb.checksum = swap_checksum(&sb);
            swap_write_slot(dev->blockdev_id, 0, &sb);
        }
    }

    spinlock_release(&swap_global_lock);
    return 0;
}

/* ── Statistics ────────────────────────────────────────────────────── */

void swap_stats(int *out_devices, uint32_t *out_total, uint32_t *out_used)
{
    int devs = 0;
    uint32_t total = 0, used = 0;

    spinlock_acquire(&swap_global_lock);
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        if (swap_devices[i].active) {
            devs++;
            total += swap_devices[i].total_slots;
            used  += swap_devices[i].used_slots;
        }
    }
    spinlock_release(&swap_global_lock);

    if (out_devices) *out_devices = devs;
    if (out_total)   *out_total   = total;
    if (out_used)    *out_used    = used;
}

const char *swap_device_path(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= SWAP_MAX_DEVICES)
        return NULL;
    if (!swap_devices[dev_idx].active)
        return NULL;
    return swap_devices[dev_idx].path;
}

/* ── EXPORT_SYMBOL ─────────────────────────────────────────────────── */

EXPORT_SYMBOL(swap_swapon);
EXPORT_SYMBOL(swap_swapoff);
EXPORT_SYMBOL(swap_out);
EXPORT_SYMBOL(swap_in);
EXPORT_SYMBOL(swap_free_slot);

/* ── Stub: swap_readpage ───────────────────────────────────────────── */
int swap_readpage(void *page)
{
    (void)page;
    kprintf("[SWAP] swap_readpage: not yet implemented\n");
    return 0;
}

/* ── Stub: swap_writepage ──────────────────────────────────────────── */
int swap_writepage(void *page)
{
    (void)page;
    kprintf("[SWAP] swap_writepage: not yet implemented\n");
    return 0;
}

/* ── Stub: swap_activate ───────────────────────────────────────────── */
int swap_activate(void)
{
    kprintf("[SWAP] swap_activate: not yet implemented\n");
    return 0;
}

/* ── Stub: swap_deactivate ─────────────────────────────────────────── */
void swap_deactivate(void)
{
    kprintf("[SWAP] swap_deactivate: not yet implemented\n");
}
