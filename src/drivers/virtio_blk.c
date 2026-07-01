/*
 * virtio-blk driver with multi-queue (virtio 1.1+)
 *
 * Supports single queue (legacy) and multi-queue (VIRTIO_BLK_F_MQ).
 * When MQ is negotiated, each vCPU gets a dedicated virtqueue for
 * lock-free I/O submission.  Fallback to single queue otherwise.
 *
 * Device: legacy PCI device 1AF4:1001 (QEMU -drive if=virtio)
 *
 * Item 195: Virtio-blk multi-queue
 */

#include "blockdev.h"
#include "virtio_blk.h"
#include "virtio.h"
#include "pci.h"
#include "printf.h"
#include "io.h"
#include "types.h"
#include "string.h"
#include "smp.h"
#include "errno.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Constants ──────────────────────────────────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_DEVICE      0x1001

/* Capacity registers (legacy config at PCI I/O offset 0x14) */
#define VIRTIO_BLK_CAPACITY_LO  0x14
#define VIRTIO_BLK_CAPACITY_HI  0x18

/* Extended config offsets (legacy PCI, after capacity at 0x14):
 * capacity(8) @ 0x14-0x1B, size_max(4) @ 0x1C, seg_max(4) @ 0x20,
 * geometry(4) @ 0x24, blk_size(4) @ 0x28, topology(8) @ 0x2C,
 * writeback(1) @ 0x34, unused0(1) @ 0x35, num_queues(2) @ 0x36,
 * max_discard_sectors(4) @ 0x38, max_discard_seg(4) @ 0x3C,
 * discard_sector_alignment(4) @ 0x40, max_write_zeroes_sectors(4) @ 0x44,
 * max_write_zeroes_seg(4) @ 0x48, write_zeroes_may_unmap(1) @ 0x4C,
 * unused1(3) @ 0x4D, max_lifetime_discard_sectors(4) @ 0x50,
 * max_segment_lifetime(4) @ 0x54, max_total_lifetime(4) @ 0x58,
 * iops_max(4) @ 0x5C, iops_min(4) @ 0x60, iops_wr_max(4) @ 0x64,
 * iops_wr_min(4) @ 0x68
 */
#define VIRTIO_BLK_CFG_SIZE_MAX         0x1C
#define VIRTIO_BLK_CFG_SEG_MAX          0x20
#define VIRTIO_BLK_CFG_BLK_SIZE         0x28
#define VIRTIO_BLK_CFG_NUM_QUEUES       0x36
#define VIRTIO_BLK_CFG_MAX_DISCARD      0x38
#define VIRTIO_BLK_CFG_MAX_DISCARD_SEG  0x3C
#define VIRTIO_BLK_CFG_DISCARD_ALIGN    0x40
#define VIRTIO_BLK_CFG_MAX_WZ           0x44
#define VIRTIO_BLK_CFG_MAX_WZ_SEG       0x48
#define VIRTIO_BLK_CFG_WZ_UNMAP         0x4C
#define VIRTIO_BLK_CFG_MAX_LIFE_DISCARD 0x50
#define VIRTIO_BLK_CFG_MAX_SEG_LIFE     0x54
#define VIRTIO_BLK_CFG_MAX_TOTAL_LIFE   0x58
#define VIRTIO_BLK_CFG_IOPS_MAX         0x5C
#define VIRTIO_BLK_CFG_IOPS_MIN         0x60
#define VIRTIO_BLK_CFG_IOPS_WR_MAX      0x64
#define VIRTIO_BLK_CFG_IOPS_WR_MIN      0x68

/* Maximum number of virtqueues we support (must be <= device max_queues) */
#define VBLK_MAX_QUEUES        8
/* Size of each virtqueue (power of 2) */
#define VRING_SIZE             16
/* Memory per queue: enough for vring_desc[16] + avail + used + header/status */
#define QUEUE_MEM_SIZE         4096

/* Descriptor flags */
#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

/* Request types */
#define VIRTIO_BLK_T_IN            0
#define VIRTIO_BLK_T_OUT           1
#define VIRTIO_BLK_T_FLUSH         4
#define VIRTIO_BLK_T_DISCARD       11
#define VIRTIO_BLK_T_WRITE_ZEROES  13

/* Discard / write-zeroes flags */
#define VIRTIO_BLK_DISCARD_F_UNMAP  (1u << 0)

/* Features the driver supports */
#define VBLK_SUPPORTED_FEATURES (VIRTIO_BLK_F_MQ | \
                                 VIRTIO_BLK_F_DISCARD | \
                                 VIRTIO_BLK_F_WRITE_ZEROES | \
                                 VIRTIO_BLK_F_LIFETIME)
/* Features the driver REQUIRES from the device */
#define VBLK_REQUIRED_FEATURES  0u

/* ── Discard/Write-zeroes descriptor (virtio spec §5.2.6.4) ───── */
#pragma pack(push, 1)
struct virtio_blk_discard_write_zeroes {
    uint64_t sector;
    uint32_t num_sectors;
    uint32_t flags;
};
#pragma pack(pop)

/* ── Virtio ring structures ────────────────────────────────────── */
#pragma pack(push, 1)
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

/* Request header prepended to each I/O */
#pragma pack(push, 1)
struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
#pragma pack(pop)

/* ── Per-queue descriptor ──────────────────────────────────────── */
struct vblk_queue {
    uint8_t  mem[QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
    int      initialized;

    /* Queue index in the device */
    uint16_t queue_idx;

    /* Virtual queue pointers into mem[] */
    struct vring_desc  *descs;
    struct vring_avail *avail;
    struct vring_used  *used;

    /* Last-seen used index for busy-wait completion */
    uint16_t last_used_idx;

    /* Reusable request header and status byte (one per queue) */
    struct virtio_blk_req_hdr           req_hdr;
    uint8_t                            status_byte;

    /* Reusable discard/write-zeroes descriptor (one per queue) */
    struct virtio_blk_discard_write_zeroes discard_desc;
};

/* ── Driver state ───────────────────────────────────────────────── */
static int            vblk_present  = 0;
static uint16_t       vblk_iobase   = 0;
static uint64_t       vblk_sectors  = 0;
static int            vblk_num_qs   = 0;  /* number of active queues */
static int            vblk_num_cpus = 1;  /* detected CPU count */

/* Per-queue state — indexed by queue_idx (0..num_qs-1) */
static struct vblk_queue vblk_queues[VBLK_MAX_QUEUES];

/* Extended config values (life-time / IOPS) */
static uint32_t vblk_max_discard_sectors;
static uint32_t vblk_max_discard_seg;
static uint32_t vblk_discard_sector_alignment;
static uint32_t vblk_max_write_zeroes_sectors;
static uint32_t vblk_max_write_zeroes_seg;
static uint8_t  vblk_write_zeroes_may_unmap;
static uint32_t vblk_max_lifetime_discard_sectors;
static uint32_t vblk_max_segment_lifetime;
static uint32_t vblk_max_total_lifetime;
static uint32_t vblk_iops_max;
static uint32_t vblk_iops_min;
static uint32_t vblk_iops_wr_max;
static uint32_t vblk_iops_wr_min;

/* ── I/O port helpers ───────────────────────────────────────────── */
static inline void vb_outb(uint8_t off, uint8_t v)  { outb(vblk_iobase + off, v); }
static inline void vb_outw(uint8_t off, uint16_t v) { outw(vblk_iobase + off, v); }
static inline void vb_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(vblk_iobase + off),     (uint8_t)v);
    outb((uint16_t)(vblk_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(vblk_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(vblk_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vb_inb(uint8_t off)  { return inb(vblk_iobase + off); }
static inline uint16_t vb_inw(uint8_t off)  { return inw(vblk_iobase + off); }
static inline uint32_t vb_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(vblk_iobase + off)) |
           ((uint32_t)inb((uint16_t)(vblk_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(vblk_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(vblk_iobase + off + 3)) << 24);
}

/* ── Queue helper: set the active queue index in the device ────── */
static void vblk_select_queue(uint16_t idx) {
    vb_outw(VIRTIO_PCI_QUEUE_SEL, idx);
}

/* ── Probe device for available queues ────────────────────────────
 * After feature negotiation (including MQ), probe each queue index
 * by selecting it and reading QUEUE_SIZE.  A queue exists if the
 * device returns a non-zero queue size.
 * Returns the number of queues found (capped at VBLK_MAX_QUEUES).
 */
static int vblk_probe_queues(void) {
    int count = 0;
    for (int i = 0; i < VBLK_MAX_QUEUES; i++) {
        vblk_select_queue((uint16_t)i);
        uint16_t qsz = vb_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (qsz == 0)
            break; /* No more queues */
        count++;
    }
    /* At minimum we must have queue 0 */
    if (count == 0) {
        kprintf("virtio-blk: ERROR - no queues available!\n");
        return -1;
    }
    return count;
}

/* ── Initialize a single virtqueue at the given index ──────────── */
static int vblk_init_queue(int qid) {
    if (qid < 0 || qid >= VBLK_MAX_QUEUES)
        return -1;

    struct vblk_queue *q = &vblk_queues[qid];
    memset(q, 0, sizeof(*q));

    q->queue_idx = (uint16_t)qid;

    /* Select the queue in the device */
    vblk_select_queue((uint16_t)qid);

    /* Point the device at our queue memory */
    uint64_t phys = (uint64_t)(uintptr_t)q->mem;
    vb_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

    /* Set up ring pointers into the queue memory */
    q->descs = (struct vring_desc *)q->mem;
    q->avail = (struct vring_avail *)(q->mem +
                  sizeof(struct vring_desc) * VRING_SIZE);
    q->used  = (struct vring_used  *)(q->mem + 2048);

    q->last_used_idx = 0;
    q->initialized   = 1;

    return 0;
}

/* ── Init ───────────────────────────────────────────────────────── */
int __init virtio_blk_init(void) {
    struct pci_device dev;

    /* Find device */
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE, &dev) < 0)
        return -1;

    vblk_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!vblk_iobase) return -1;

    pci_enable_bus_master(&dev);

    /* Reset device */
    vb_outb(VIRTIO_PCI_STATUS, 0);

    /* Acknowledge + driver OK */
    vb_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features (includes MQ if device supports it) */
    if (virtio_negotiate_features_ex(vb_inl, vb_outl, vb_outb, vb_inb,
                                     VBLK_SUPPORTED_FEATURES,
                                     VBLK_REQUIRED_FEATURES,
                                     virtio_blk_features,
                                     "virtio-blk") < 0) {
        kprintf("virtio-blk: device rejected feature negotiation\n");
        return -1;
    }

    /* Read capacity */
    uint64_t cap_lo = vb_inl(VIRTIO_BLK_CAPACITY_LO);
    uint64_t cap_hi = vb_inl(VIRTIO_BLK_CAPACITY_HI);
    vblk_sectors = (cap_hi << 32) | cap_lo;

    /* Read extended config values (life-time / IOPS) */
    vblk_max_discard_sectors        = vb_inl(VIRTIO_BLK_CFG_MAX_DISCARD);
    vblk_max_discard_seg            = vb_inl(VIRTIO_BLK_CFG_MAX_DISCARD_SEG);
    vblk_discard_sector_alignment   = vb_inl(VIRTIO_BLK_CFG_DISCARD_ALIGN);
    vblk_max_write_zeroes_sectors   = vb_inl(VIRTIO_BLK_CFG_MAX_WZ);
    vblk_max_write_zeroes_seg       = vb_inl(VIRTIO_BLK_CFG_MAX_WZ_SEG);
    vblk_write_zeroes_may_unmap     = vb_inb(VIRTIO_BLK_CFG_WZ_UNMAP);
    vblk_max_lifetime_discard_sectors = vb_inl(VIRTIO_BLK_CFG_MAX_LIFE_DISCARD);
    vblk_max_segment_lifetime       = vb_inl(VIRTIO_BLK_CFG_MAX_SEG_LIFE);
    vblk_max_total_lifetime         = vb_inl(VIRTIO_BLK_CFG_MAX_TOTAL_LIFE);
    vblk_iops_max                   = vb_inl(VIRTIO_BLK_CFG_IOPS_MAX);
    vblk_iops_min                   = vb_inl(VIRTIO_BLK_CFG_IOPS_MIN);
    vblk_iops_wr_max                = vb_inl(VIRTIO_BLK_CFG_IOPS_WR_MAX);
    vblk_iops_wr_min                = vb_inl(VIRTIO_BLK_CFG_IOPS_WR_MIN);

    /* Detect number of CPUs */
    vblk_num_cpus = smp_get_cpu_count();
    if (vblk_num_cpus < 1) vblk_num_cpus = 1;

    /* Probe for available queues */
    int avail_queues = vblk_probe_queues();
    if (avail_queues < 0) return -1;

    /* Determine how many queues to use: min(num_cpus, avail_queues, MAX) */
    int want_queues = vblk_num_cpus;
    if (want_queues > VBLK_MAX_QUEUES)
        want_queues = VBLK_MAX_QUEUES;
    if (want_queues > avail_queues)
        want_queues = avail_queues;

    /* Initialize each queue */
    for (int i = 0; i < want_queues; i++) {
        if (vblk_init_queue(i) < 0) {
            kprintf("virtio-blk: failed to init queue %d\n", i);
            /* Continue with fewer queues — at least queue 0 should work */
            want_queues = i > 0 ? i : 1;
            break;
        }
    }

    vblk_num_qs = want_queues;

    /* Driver OK — device can start processing */
    vb_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_DRIVER_OK);

    vblk_present = 1;

    kprintf("virtio-blk: initialized (iobase=0x%x, sectors=%llu, "
            "queues=%d, cpus=%d, discard=%uK, wz=%uK, "
            "life_discard=%u, seg_life=%u, total_life=%u, "
            "iops_rw=%u/%u/%u/%u)\n",
            (unsigned int)vblk_iobase, vblk_sectors,
            vblk_num_qs, vblk_num_cpus,
            (unsigned int)(vblk_max_discard_sectors * 512 / 1024),
            (unsigned int)(vblk_max_write_zeroes_sectors * 512 / 1024),
            (unsigned int)vblk_max_lifetime_discard_sectors,
            (unsigned int)vblk_max_segment_lifetime,
            (unsigned int)vblk_max_total_lifetime,
            (unsigned int)vblk_iops_min,
            (unsigned int)vblk_iops_max,
            (unsigned int)vblk_iops_wr_min,
            (unsigned int)vblk_iops_wr_max);
    return 0;
}

#include "initcall.h"
device_initcall(virtio_blk_init);

/* ── I/O request on a specific queue ──────────────────────────────
 * Submits a read or write request on the given queue index.
 * Busy-waits for completion (this driver has no IRQ handler yet).
 * Returns 0 on success, -1 on error.
 */
static int vblk_queue_request(int qid, uint32_t type,
                               uint64_t lba, uint32_t count, void *buf) {
    if (!vblk_present || qid < 0 || qid >= vblk_num_qs)
        return -1;

    struct vblk_queue *q = &vblk_queues[qid];
    if (!q->initialized)
        return -1;

    struct vring_desc  *descs = q->descs;
    struct vring_avail *avail = q->avail;
    struct vring_used  *used  = q->used;

    /* Prepare request header */
    q->req_hdr.type     = type;
    q->req_hdr.reserved = 0;
    q->req_hdr.sector   = lba;
    q->status_byte      = 0xFF;

    /* Build descriptor chain (3 descriptors: header, data, status) */
    descs[0].addr  = (uint64_t)(uintptr_t)&q->req_hdr;
    descs[0].len   = sizeof(q->req_hdr);
    descs[0].flags = VRING_DESC_F_NEXT;
    descs[0].next  = 1;

    descs[1].addr  = (uint64_t)(uintptr_t)buf;
    descs[1].len   = count * 512;
    descs[1].flags = VRING_DESC_F_NEXT |
                     (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    descs[1].next  = 2;

    descs[2].addr  = (uint64_t)(uintptr_t)&q->status_byte;
    descs[2].len   = 1;
    descs[2].flags = VRING_DESC_F_WRITE;
    descs[2].next  = 0;

    /* Submit to the avail ring */
    uint16_t prev_used = used->idx;
    uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[avail_idx] = 0;  /* descriptor index 0 (head of chain) */
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device — must select the queue first */
    vblk_select_queue(q->queue_idx);
    vb_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->queue_idx);

    /* Busy-wait for completion (softirq-based completion planned) */
    uint32_t timeout = 100000;
    while (used->idx == prev_used && timeout--) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        kprintf("virtio-blk: timeout on queue %d lba=%llu count=%u\n",
                qid, lba, count);
        return -1;
    }

    return (q->status_byte == 0) ? 0 : -1;
}

/* ── Discard / write-zeroes request on a specific queue ──────────
 * Submits a discard or write-zeroes command on the given queue.
 * Uses a 3-descriptor chain:
 *   [0] request header (type + reserved + sector)
 *   [1] discard/write-zeroes descriptor (sector + count + flags)
 *   [2] status byte
 * Descriptor [1] is device-readable (OUT) — the device reads it.
 * Busy-waits for completion.
 * Returns 0 on success, -1 on error.
 */
static int vblk_queue_special(int qid, uint32_t type,
                               uint64_t lba, uint32_t count, uint32_t flags)
{
    if (!vblk_present || qid < 0 || qid >= vblk_num_qs)
        return -1;

    struct vblk_queue *q = &vblk_queues[qid];
    if (!q->initialized)
        return -1;

    struct vring_desc  *descs = q->descs;
    struct vring_avail *avail = q->avail;
    struct vring_used  *used  = q->used;

    /* Prepare request header */
    q->req_hdr.type     = type;
    q->req_hdr.reserved = 0;
    q->req_hdr.sector   = lba;
    q->status_byte      = 0xFF;

    /* Prepare discard/write-zeroes descriptor */
    q->discard_desc.sector      = lba;
    q->discard_desc.num_sectors = count;
    q->discard_desc.flags       = flags;

    /* Build descriptor chain (3 descriptors: header, discard_desc, status) */
    descs[0].addr  = (uint64_t)(uintptr_t)&q->req_hdr;
    descs[0].len   = sizeof(q->req_hdr);
    descs[0].flags = VRING_DESC_F_NEXT;
    descs[0].next  = 1;

    descs[1].addr  = (uint64_t)(uintptr_t)&q->discard_desc;
    descs[1].len   = sizeof(q->discard_desc);
    descs[1].flags = VRING_DESC_F_NEXT;  /* device-readable (OUT), no VRING_DESC_F_WRITE */
    descs[1].next  = 2;

    descs[2].addr  = (uint64_t)(uintptr_t)&q->status_byte;
    descs[2].len   = 1;
    descs[2].flags = VRING_DESC_F_WRITE;
    descs[2].next  = 0;

    /* Submit to the avail ring */
    uint16_t prev_used = used->idx;
    uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[avail_idx] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    vblk_select_queue(q->queue_idx);
    vb_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->queue_idx);

    /* Busy-wait for completion */
    uint32_t timeout = 100000;
    while (used->idx == prev_used && timeout--) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        kprintf("virtio-blk: timeout on special cmd type=%u lba=%llu count=%u\n",
                (unsigned int)type, lba, count);
        return -1;
    }

    return (q->status_byte == 0) ? 0 : -1;
}

/* ── Select the queue for the current CPU ──────────────────────── */
static int vblk_current_queue(void) {
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= vblk_num_qs)
        cpu = 0; /* fallback to queue 0 */
    return cpu;
}

/* ── Public API ─────────────────────────────────────────────────── */
int virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    int qid = vblk_current_queue();
    return vblk_queue_request(qid, VIRTIO_BLK_T_IN, lba, count, buf);
}

int virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    int qid = vblk_current_queue();
    return vblk_queue_request(qid, VIRTIO_BLK_T_OUT, lba, count, (void *)buf);
}

int virtio_blk_discard_sectors(uint64_t lba, uint32_t count) {
    int qid = vblk_current_queue();
    return vblk_queue_special(qid, VIRTIO_BLK_T_DISCARD, lba, count,
                               VIRTIO_BLK_DISCARD_F_UNMAP);
}

int virtio_blk_write_zeroes_sectors(uint64_t lba, uint32_t count) {
    int qid = vblk_current_queue();
    return vblk_queue_special(qid, VIRTIO_BLK_T_WRITE_ZEROES, lba, count, 0);
}

uint64_t virtio_blk_sector_count(void) {
    return vblk_sectors;
}

/* ── Life-time / IOPS config getters ─────────────────────────────── */
uint32_t virtio_blk_get_max_discard_sectors(void) {
    return vblk_max_discard_sectors;
}

uint32_t virtio_blk_get_max_discard_seg(void) {
    return vblk_max_discard_seg;
}

uint32_t virtio_blk_get_discard_sector_alignment(void) {
    return vblk_discard_sector_alignment;
}

uint32_t virtio_blk_get_max_write_zeroes_sectors(void) {
    return vblk_max_write_zeroes_sectors;
}

uint32_t virtio_blk_get_max_write_zeroes_seg(void) {
    return vblk_max_write_zeroes_seg;
}

uint8_t virtio_blk_get_write_zeroes_may_unmap(void) {
    return vblk_write_zeroes_may_unmap;
}

uint32_t virtio_blk_get_max_lifetime_discard_sectors(void) {
    return vblk_max_lifetime_discard_sectors;
}

uint32_t virtio_blk_get_max_segment_lifetime(void) {
    return vblk_max_segment_lifetime;
}

uint32_t virtio_blk_get_max_total_lifetime(void) {
    return vblk_max_total_lifetime;
}

uint32_t virtio_blk_get_iops_max(void) {
    return vblk_iops_max;
}

uint32_t virtio_blk_get_iops_min(void) {
    return vblk_iops_min;
}

uint32_t virtio_blk_get_iops_wr_max(void) {
    return vblk_iops_wr_max;
}

uint32_t virtio_blk_get_iops_wr_min(void) {
    return vblk_iops_wr_min;
}

/* ── Block-device layer submit function ──────────────────────────
 * Handles READ, WRITE, and DISCARD requests submitted via the
 * block device layer (blockdev_discard() → blk_submit_async → this).
 * WRITE_ZEROES requests can be submitted directly through
 * virtio_blk_write_zeroes_sectors().
 */
static int vblk_submit(struct blk_request *req) {
    if (!req)
        return -EINVAL;

    int qid = vblk_current_queue();

    /* Handle discard */
    if (req->flags & BLK_REQ_DISCARD) {
        int ret = vblk_queue_special(qid, VIRTIO_BLK_T_DISCARD,
                                      req->lba, req->count,
                                      VIRTIO_BLK_DISCARD_F_UNMAP);
        req->result = ret;
        return ret;
    }

    /* Handle read/write */
    uint32_t type;
    if (req->flags & BLK_REQ_READ)
        type = VIRTIO_BLK_T_IN;
    else if (req->flags & BLK_REQ_WRITE)
        type = VIRTIO_BLK_T_OUT;
    else {
        req->result = -EINVAL;
        return -EINVAL;
    }

    int ret = vblk_queue_request(qid, type, req->lba, req->count, req->buf);
    if (ret < 0)
        req->result = -EIO;
    else
        req->result = 0;
    return ret < 0 ? ret : 0;
}

void virtio_blk_register_blockdev(void) {
    if (!vblk_present) return;
    blockdev_register(BLOCKDEV_VIRTIO0, "virtio0",
                      vblk_submit, NULL, vblk_sectors, 0);
}

/* ── Module hooks ──────────────────────────────────────────────── */
#ifdef MODULE
int init_module(void) {
    if (virtio_blk_init() == 0) {
        virtio_blk_register_blockdev();
        kprintf("[OK] virtio-blk (module): %llu sectors, %d queues\n",
                virtio_blk_sector_count(), vblk_num_qs);
        return 0;
    }
    return -1;
}

void cleanup_module(void) {
    vblk_present = 0;
    kprintf("virtio-blk (module): unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO block device driver with multi-queue (Item 195)");
MODULE_ALIAS("pci:v00001AF4d00001001sv*sd*bc*sc*i*");
#endif /* MODULE */

/* Forward declaration for block-device layer API */
struct block_device;

/* ── Block-device layer read (delegates to sector API) ── */
int virtio_blk_read(struct block_device *dev, uint64_t sector, void *buf, int count)
{
    (void)dev;
    return virtio_blk_read_sectors(sector, (uint32_t)count, buf);
}

/* ── Block-device layer write (delegates to sector API) ─ */
int virtio_blk_write(struct block_device *dev, uint64_t sector, const void *buf, int count)
{
    (void)dev;
    return virtio_blk_write_sectors(sector, (uint32_t)count, buf);
}

