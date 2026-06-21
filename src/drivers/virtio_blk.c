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
#ifdef MODULE
#include "module.h"
#endif

/* ── Constants ──────────────────────────────────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_BLK_DEVICE      0x1001

/* Capacity registers (legacy config at PCI I/O offset 0x14) */
#define VIRTIO_BLK_CAPACITY_LO  0x14
#define VIRTIO_BLK_CAPACITY_HI  0x18

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
#define VIRTIO_BLK_T_IN        0
#define VIRTIO_BLK_T_OUT       1

/* Features the driver supports */
#define VBLK_SUPPORTED_FEATURES (VIRTIO_BLK_F_MQ)
/* Features the driver REQUIRES from the device */
#define VBLK_REQUIRED_FEATURES  0u

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
    struct virtio_blk_req_hdr req_hdr;
    uint8_t                  status_byte;
};

/* ── Driver state ───────────────────────────────────────────────── */
static int            vblk_present  = 0;
static uint16_t       vblk_iobase   = 0;
static uint64_t       vblk_sectors  = 0;
static int            vblk_num_qs   = 0;  /* number of active queues */
static int            vblk_num_cpus = 1;  /* detected CPU count */

/* Per-queue state — indexed by queue_idx (0..num_qs-1) */
static struct vblk_queue vblk_queues[VBLK_MAX_QUEUES];

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
int virtio_blk_init(void) {
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
            "queues=%d, cpus=%d)\n",
            (unsigned int)vblk_iobase, vblk_sectors,
            vblk_num_qs, vblk_num_cpus);
    return 0;
}

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

uint64_t virtio_blk_sector_count(void) {
    return vblk_sectors;
}

/* ── Block device registration ─────────────────────────────────── */
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
    blockdev_register_legacy(BLOCKDEV_VIRTIO0, "virtio0",
                      vblk_bd_read, vblk_bd_write, vblk_bd_size);
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

