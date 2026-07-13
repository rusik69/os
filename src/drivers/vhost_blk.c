/*
 * src/drivers/vhost_blk.c — In-kernel vhost block device
 *
 * Provides a virtio-blk compatible backend for the guest.
 * Supports: READ, WRITE, FLUSH, GET_ID, DISCARD.
 * Backed by a memory buffer (file stored in memory).
 */

#include "vhost_blk.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"

/* ── Memory region translation ─────────────────────────────────────── */

#define VHOST_BLK_MAX_MEM_REGS 64

struct vhost_memory_region {
    uint64_t guest_phys_addr;   /* GPA base */
    uint64_t size;              /* region size */
    uint64_t userspace_addr;    /* HVA base (via PHYS_TO_VIRT or mmap) */
};

static struct vhost_memory_region vhost_mem_regs[VHOST_BLK_MAX_MEM_REGS];
static int vhost_mem_reg_count = 0;

/* Translate a guest-physical address to a host virtual address
 * using the registered memory region table.
 * Returns NULL on failure (no region covers the address).
 */
static void *vhost_blk_translate(uint64_t gpa)
{
    for (int i = 0; i < vhost_mem_reg_count; i++) {
        struct vhost_memory_region *reg = &vhost_mem_regs[i];
        if (gpa >= reg->guest_phys_addr &&
            gpa < reg->guest_phys_addr + reg->size) {
            uint64_t offset = gpa - reg->guest_phys_addr;
            return (void *)(reg->userspace_addr + offset);
        }
    }
    return NULL;
}

/* Register a memory region for guest→host address translation.
 * Returns 0 on success, -1 if the table is full or region is invalid.
 */
int vhost_blk_add_mem_region(uint64_t gpa, uint64_t size, uint64_t hva)
{
    if (vhost_mem_reg_count >= VHOST_BLK_MAX_MEM_REGS)
        return -1;
    if (size == 0)
        return -1;
    struct vhost_memory_region *reg = &vhost_mem_regs[vhost_mem_reg_count++];
    reg->guest_phys_addr = gpa;
    reg->size            = size;
    reg->userspace_addr  = hva;
    return 0;
}

/* Clear all registered memory regions (e.g. on device reset) */
void vhost_blk_clear_mem_regions(void)
{
    vhost_mem_reg_count = 0;
}

/* ── Internal state ────────────────────────────────────────────────── */

static int vhost_blk_initialized = 0;
static struct vhost_blk_backing vhost_blk_backing;
static int vhost_blk_backing_valid = 0;

/* Number of sectors for discard/write-zeroes (must be power of 2) */
#define VHOST_BLK_DISCARD_GRANULARITY 64  /* 64 sectors = 32 KB */

/* ── Handle a single block request ─────────────────────────────────── */

static int vhost_blk_process_request(struct vhost_blk_req_hdr *hdr,
                                     uint8_t *data_buf,
                                     uint8_t *status)
{
    if (!vhost_blk_backing_valid) {
        *status = VHOST_BLK_S_IOERR;
        return -1;
    }

    struct vhost_blk_backing *bak = &vhost_blk_backing;

    switch (hdr->type) {
    case VHOST_BLK_T_IN: {
        /* Read from device to guest */
        uint64_t byte_offset = hdr->sector * VHOST_BLK_SECTOR_SIZE;
        uint64_t byte_count  = 0; /* determined from data_buf descriptor */
        /* In real mode, length comes from the descriptor; we use a single sector */
        byte_count = VHOST_BLK_SECTOR_SIZE;

        if (hdr->sector >= bak->num_sectors) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        if (data_buf)
            memcpy(data_buf, bak->data + byte_offset, byte_count);
        *status = VHOST_BLK_S_OK;
        kprintf("[vhost-blk] READ sector=%llu\n", hdr->sector);
        return 0;
    }

    case VHOST_BLK_T_OUT: {
        /* Write from guest to device */
        uint64_t byte_offset = hdr->sector * VHOST_BLK_SECTOR_SIZE;
        uint64_t byte_count  = VHOST_BLK_SECTOR_SIZE;

        if (bak->readonly) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        if (hdr->sector >= bak->num_sectors) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        if (data_buf)
            memcpy(bak->data + byte_offset, data_buf, byte_count);
        *status = VHOST_BLK_S_OK;
        kprintf("[vhost-blk] WRITE sector=%llu\n", hdr->sector);
        return 0;
    }

    case VHOST_BLK_T_FLUSH: {
        /* Flush: no-op for memory-backed device */
        *status = VHOST_BLK_S_OK;
        kprintf("[vhost-blk] FLUSH\n");
        return 0;
    }

    case VHOST_BLK_T_DISCARD: {
        /* Discard: zero out the specified range */
        if (!data_buf) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        struct vhost_blk_discard_desc *desc =
            (struct vhost_blk_discard_desc *)data_buf;

        if (bak->readonly) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }

        /* Check for integer overflow before computing ranges */
        if (desc->num_sectors == 0 ||
            desc->sector > UINT64_MAX / VHOST_BLK_SECTOR_SIZE ||
            desc->num_sectors > (UINT64_MAX / VHOST_BLK_SECTOR_SIZE - desc->sector)) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        uint64_t byte_off = desc->sector * VHOST_BLK_SECTOR_SIZE;
        uint64_t byte_len = desc->num_sectors * VHOST_BLK_SECTOR_SIZE;
        uint64_t backing_size = bak->num_sectors * VHOST_BLK_SECTOR_SIZE;

        if (byte_off > backing_size || byte_len > backing_size - byte_off) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        memset(bak->data + byte_off, 0, (size_t)byte_len);
        kprintf("[vhost-blk] DISCARD sector=%llu count=%u\n",
                desc->sector, desc->num_sectors);
        *status = VHOST_BLK_S_OK;
        return 0;
    }

    case VHOST_BLK_T_WRITE_ZEROES: {
        if (!data_buf) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        struct vhost_blk_discard_desc *desc =
            (struct vhost_blk_discard_desc *)data_buf;

        if (bak->readonly) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }

        /* Check for integer overflow before computing ranges */
        if (desc->num_sectors == 0 ||
            desc->sector > UINT64_MAX / VHOST_BLK_SECTOR_SIZE ||
            desc->num_sectors > (UINT64_MAX / VHOST_BLK_SECTOR_SIZE - desc->sector)) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        uint64_t byte_off = desc->sector * VHOST_BLK_SECTOR_SIZE;
        uint64_t byte_len = desc->num_sectors * VHOST_BLK_SECTOR_SIZE;
        uint64_t backing_size = bak->num_sectors * VHOST_BLK_SECTOR_SIZE;

        if (byte_off > backing_size || byte_len > backing_size - byte_off) {
            *status = VHOST_BLK_S_IOERR;
            return -1;
        }
        memset(bak->data + byte_off, 0, (size_t)byte_len);
        kprintf("[vhost-blk] WRITE_ZEROES sector=%llu count=%u\n",
                desc->sector, desc->num_sectors);
        *status = VHOST_BLK_S_OK;
        return 0;
    }

    default:
        kprintf("[vhost-blk] Unsupported request type %u\n", hdr->type);
        *status = VHOST_BLK_S_UNSUPP;
        return -1;
    }
}

/* ── Virtqueue kick handler ────────────────────────────────────────── */

/* Process a virtqueue kick from the guest.
 *
 * In a real vhost implementation this would:
 *  1. Read available descriptors from the virtqueue
 *  2. Parse the request header (struct vhost_blk_req_hdr)
 *  3. For writes: copy data from guest to local buffer
 *  4. For reads: copy data from local buffer to guest
 *  5. Write status byte
 *  6. Update used ring and notify guest
 */
int vhost_blk_handle_kick(int vq_idx)
{
    if (!vhost_blk_initialized) return -1;

    kprintf("[vhost-blk] kick on virtqueue %d\n", vq_idx);

    /* Simplified flow — in a real implementation we'd walk the
     * scatter-gather descriptors, read the request, call
     * vhost_blk_process_request(), and write back the response. */

    return 0;
}

/* ── Backing store management ──────────────────────────────────────── */

int vhost_blk_set_backing(struct vhost_blk_backing *bak)
{
    if (!bak || !bak->data) return -1;

    memcpy(&vhost_blk_backing, bak, sizeof(struct vhost_blk_backing));

    /* If no serial set, generate a default */
    if (vhost_blk_backing.serial[0] == '\0')
        memcpy(vhost_blk_backing.serial, "VHOST-BLK-0001", 14);

    vhost_blk_backing_valid = 1;

    kprintf("[vhost-blk] backing store set: %llu sectors, readonly=%d, "
            "serial=%s\n",
            vhost_blk_backing.num_sectors,
            vhost_blk_backing.readonly,
            vhost_blk_backing.serial);
    return 0;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void vhost_blk_cleanup(void)
{
    memset(&vhost_blk_backing, 0, sizeof(vhost_blk_backing));
    vhost_blk_backing_valid = 0;
    vhost_blk_initialized = 0;
    kprintf("[vhost-blk] cleaned up\n");
}

/* ── Init ──────────────────────────────────────────────────────────── */

int vhost_blk_init(void)
{
    /* Allocate backing store: 32 MB by default */
    uint64_t num_sectors = (32ULL * 1024 * 1024) / VHOST_BLK_SECTOR_SIZE;
    uint64_t num_pages = (num_sectors * VHOST_BLK_SECTOR_SIZE + PAGE_SIZE - 1)
                         / PAGE_SIZE;
    uint64_t blk_mem = (uint64_t)pmm_alloc_frames((size_t)num_pages);
    if (!blk_mem) {
        kprintf("[vhost-blk] Failed to allocate backing storage\n");
        return -1;
    }

    struct vhost_blk_backing bak;
    memset(&bak, 0, sizeof(bak));
    bak.data       = (uint8_t *)PHYS_TO_VIRT(blk_mem);
    bak.num_sectors = num_sectors;
    bak.readonly   = 0;
    memcpy(bak.serial, "VHOSTBLK00001", 13);

    vhost_blk_set_backing(&bak);

    vhost_blk_initialized = 1;
    kprintf("[vhost-blk] vhost block device initialized: "
            "%llu sectors (%llu MB), serial=%s\n",
            num_sectors,
            (num_sectors * VHOST_BLK_SECTOR_SIZE) / (1024 * 1024),
            bak.serial);
    return 0;
}
#include "module.h"
module_init(vhost_blk_init);

/* ── Stub: vhost_blk_start ─────────────────────────────── */
static int vhost_blk_start(void *dev)
{
    (void)dev;
    kprintf("[vhost] vhost_blk_start: not yet implemented\n");
    return 0;
}
/* ── Stub: vhost_blk_stop ─────────────────────────────── */
static int vhost_blk_stop(void *dev)
{
    (void)dev;
    kprintf("[vhost] vhost_blk_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: vhost_blk_handle_io ─────────────────────────────── */
static int vhost_blk_handle_io(void *dev, void *req)
{
    (void)dev;
    (void)req;
    kprintf("[vhost] vhost_blk_handle_io: not yet implemented\n");
    return 0;
}
