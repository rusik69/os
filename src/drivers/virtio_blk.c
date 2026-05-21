/*
 * virtio-blk driver  (legacy PCI device 1AF4:1001)
 *
 * Same caveat as virtio-net: QEMU's default disk is an IDE/ATA device.
 * This driver activates only when QEMU is launched with:
 *   -drive file=disk.img,if=virtio
 */

#include "blockdev.h"
#include "virtio_blk.h"
#include "pci.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "types.h"

/* ── Virtio PCI offsets (same as virtio-net) ────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_DEVICE      0x1001

#define VIRTIO_PCI_HOST_FEAT    0x00
#define VIRTIO_PCI_GUEST_FEAT   0x04
#define VIRTIO_PCI_QUEUE_PFN    0x08
#define VIRTIO_PCI_QUEUE_SIZE   0x0C
#define VIRTIO_PCI_QUEUE_SEL    0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10
#define VIRTIO_PCI_STATUS       0x12
#define VIRTIO_PCI_ISR          0x13
/* blk-specific config starts at 0x14 */
#define VIRTIO_BLK_CAPACITY_LO  0x14
#define VIRTIO_BLK_CAPACITY_HI  0x18

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4

#define VRING_SIZE    16
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* ── virtio-blk request types ───────────────────────────────────── */
#define VIRTIO_BLK_T_IN  0   /* read */
#define VIRTIO_BLK_T_OUT 1   /* write */

#pragma pack(push, 1)
struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VRING_SIZE];
};

struct vring_used_elem { uint32_t id; uint32_t len; };
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VRING_SIZE];
};
#pragma pack(pop)

/* ── Driver state ────────────────────────────────────────────────── */
static int      vblk_present  = 0;
static uint16_t vblk_iobase   = 0;
static uint64_t vblk_sectors  = 0;

static uint8_t __attribute__((aligned(4096))) vblk_queue_mem[4096];

/* ── Helpers ─────────────────────────────────────────────────────── */
static inline void vb_outb(uint8_t off, uint8_t v)  { outb(vblk_iobase + off, v); }
static inline void vb_outw(uint8_t off, uint16_t v) { outw(vblk_iobase + off, v); }
static inline void vb_outl(uint8_t off, uint32_t v) {
    outb(vblk_iobase + off,     (uint8_t)v);
    outb(vblk_iobase + off + 1, (uint8_t)(v >> 8));
    outb(vblk_iobase + off + 2, (uint8_t)(v >> 16));
    outb(vblk_iobase + off + 3, (uint8_t)(v >> 24));
}
static inline uint8_t  vb_inb(uint8_t off)  { return inb(vblk_iobase + off); }
static inline uint16_t vb_inw(uint8_t off)  { return inw(vblk_iobase + off); }
static inline uint32_t vb_inl(uint8_t off) {
    return (uint32_t)inb(vblk_iobase + off) |
           ((uint32_t)inb(vblk_iobase + off + 1) << 8)  |
           ((uint32_t)inb(vblk_iobase + off + 2) << 16) |
           ((uint32_t)inb(vblk_iobase + off + 3) << 24);
}

/* ── Init ────────────────────────────────────────────────────────── */
int virtio_blk_init(void) {
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE, &dev) < 0)
        return -1;

    vblk_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!vblk_iobase) return -1;

    pci_enable_bus_master(&dev);

    /* Reset */
    vb_outb(VIRTIO_PCI_STATUS, 0);
    vb_outb(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    vb_outl(VIRTIO_PCI_GUEST_FEAT, vb_inb(VIRTIO_PCI_HOST_FEAT));

    /* Read capacity */
    uint64_t cap_lo = vb_inl(VIRTIO_BLK_CAPACITY_LO);
    uint64_t cap_hi = vb_inl(VIRTIO_BLK_CAPACITY_HI);
    vblk_sectors = (cap_hi << 32) | cap_lo;

    /* Set up request queue (index 0) */
    vb_outw(VIRTIO_PCI_QUEUE_SEL, 0);
    uint64_t phys = (uint64_t)(uintptr_t)vblk_queue_mem;
    vb_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

    vb_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    vblk_present = 1;
    kprintf("virtio-blk: initialized (iobase=0x%x, sectors=%llu)\n",
            (uint64_t)vblk_iobase, vblk_sectors);
    return 0;
}

/* ── I/O ─────────────────────────────────────────────────────────── */
static int vblk_request(uint32_t type, uint64_t lba, uint32_t count, void *buf) {
    if (!vblk_present) return -1;

    struct vring_desc  *descs = (struct vring_desc  *)vblk_queue_mem;
    struct vring_avail *avail = (struct vring_avail *)(vblk_queue_mem +
                                 sizeof(struct vring_desc) * VRING_SIZE);
    struct vring_used  *used  = (struct vring_used  *)(vblk_queue_mem + 2048);

    static struct virtio_blk_req_hdr req_hdr;
    req_hdr.type     = type;
    req_hdr.reserved = 0;
    req_hdr.sector   = lba;

    static uint8_t status_byte;
    status_byte = 0xFF;

    /* Desc 0: request header (read-only for device) */
    descs[0].addr  = (uint64_t)(uintptr_t)&req_hdr;
    descs[0].len   = sizeof(req_hdr);
    descs[0].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_OUT ? 0 : 0);
    descs[0].next  = 1;
    /* Desc 1: data buffer */
    descs[1].addr  = (uint64_t)(uintptr_t)buf;
    descs[1].len   = count * 512;
    descs[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    descs[1].next  = 2;
    /* Desc 2: status byte (writable by device) */
    descs[2].addr  = (uint64_t)(uintptr_t)&status_byte;
    descs[2].len   = 1;
    descs[2].flags = VRING_DESC_F_WRITE;
    descs[2].next  = 0;

    uint16_t prev_used = used->idx;
    uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[avail_idx] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    vb_outw(VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Busy-wait for completion (no IRQ handler yet) */
    uint32_t timeout = 100000;
    while (used->idx == prev_used && timeout--) {
        __asm__ volatile("pause");
    }

    return (status_byte == 0) ? 0 : -1;
}

int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    return vblk_request(VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    return vblk_request(VIRTIO_BLK_T_OUT, lba, count, (void *)buf);
}

uint64_t virtio_blk_sector_count(void) { return vblk_sectors; }

static int vblk_bd_read(uint32_t lba, uint8_t count, void *buf) {
    return virtio_blk_read_sectors(lba, count, buf);
}

static int vblk_bd_write(uint32_t lba, uint8_t count, const void *buf) {
    return virtio_blk_write_sectors(lba, count, buf);
}

static uint32_t vblk_bd_size(void) {
    uint64_t n = vblk_sectors;
    return (n > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)n;
}

void virtio_blk_register_blockdev(void) {
    if (!vblk_present) return;
    blockdev_register(BLOCKDEV_VIRTIO0, "virtio0",
                      vblk_bd_read, vblk_bd_write, vblk_bd_size);
}
