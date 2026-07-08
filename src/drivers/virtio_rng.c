/*
 * src/drivers/virtio_rng.c — VirtIO entropy driver
 *
 * Implements VirtIO RNG (random number generator)
 * for PCI device 1AF4:1005 (virtio-rng).
 * Feeds entropy into the kernel's RNG pool via rng_add_entropy().
 * Uses proper virtqueue-based random data reading.
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "rng.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_RNG_DEVICE      0x1005

/* ── Feature bits ──────────────────────────────────────────────── */

#define VIRTIO_RNG_F_RNG_EE     (1u << 0)

/* ── Ring constants ────────────────────────────────────────────── */

#define VRING_SIZE             16
#define QUEUE_MEM_SIZE         4096

/* Descriptor flags */
#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

/* ── Virtio ring structures (packed, legacy layout) ────────────── */

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

/* ── Per-device queue state ────────────────────────────────────── */

struct vrng_queue {
    uint8_t  mem[QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
    int      initialized;

    /* Queue index (always 0 for legacy RNG) */
    uint16_t queue_idx;

    /* Virtual queue pointers into mem[] */
    struct vring_desc  *descs;
    struct vring_avail *avail;
    struct vring_used  *used;

    /* Last-seen used index for busy-wait completion */
    uint16_t last_used_idx;

    /* Buffer for receiving random data */
    uint8_t  entropy_buf[64];
};

/* ── Driver state ──────────────────────────────────────────────── */

static int             rng_present = 0;
static uint16_t        rng_iobase  = 0;
static struct vrng_queue rng_queue;

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline void vrng_outb(uint8_t off, uint8_t v)  { outb(rng_iobase + off, v); }
static inline void vrng_outw(uint8_t off, uint16_t v) { outw(rng_iobase + off, v); }
static inline void vrng_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(rng_iobase + off),     (uint8_t)v);
    outb((uint16_t)(rng_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(rng_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(rng_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vrng_inb(uint8_t off)  { return inb(rng_iobase + off); }
static inline uint16_t vrng_inw(uint8_t off)  { return inw(rng_iobase + off); }
static inline uint32_t vrng_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(rng_iobase + off)) |
           ((uint32_t)inb((uint16_t)(rng_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(rng_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(rng_iobase + off + 3)) << 24);
}

/* ── Queue helpers ─────────────────────────────────────────────── */

/** Select the active queue index in the device. */
static void vrng_select_queue(uint16_t idx)
{
    vrng_outw(VIRTIO_PCI_QUEUE_SEL, idx);
}

/** Initialise the single virtqueue. */
static int vrng_init_queue(void)
{
    struct vrng_queue *q = &rng_queue;
    memset(q, 0, sizeof(*q));

    q->queue_idx = 0;

    /* Select queue 0 */
    vrng_select_queue(0);

    /* Point the device at our queue memory (physical page number) */
    uint64_t phys = (uint64_t)(uintptr_t)q->mem;
    vrng_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

    /* Set up ring pointers into the queue memory */
    q->descs = (struct vring_desc *)q->mem;
    q->avail = (struct vring_avail *)(q->mem +
                  sizeof(struct vring_desc) * VRING_SIZE);
    q->used  = (struct vring_used  *)(q->mem + 2048);

    q->last_used_idx = 0;
    q->initialized   = 1;

    kprintf("[VIRTIO-RNG] queue 0 initialised (vring_size=%d)\n", VRING_SIZE);
    return 0;
}

/** Submit a buffer to the virtqueue and wait for the device to fill it. */
static int vrng_read_entropy(void *buf, uint32_t len)
{
    struct vrng_queue *q = &rng_queue;
    if (!q->initialized || !buf || len == 0 || len > sizeof(q->entropy_buf))
        return -1;

    struct vring_desc  *descs = q->descs;
    struct vring_avail *avail = q->avail;
    struct vring_used  *used  = q->used;

    /* Build a single descriptor: device writes random data into buf */
    descs[0].addr  = (uint64_t)(uintptr_t)buf;
    descs[0].len   = len;
    descs[0].flags = VRING_DESC_F_WRITE;
    descs[0].next  = 0;

    /* Submit to the avail ring */
    uint16_t prev_used = used->idx;
    uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[avail_idx] = 0;  /* descriptor index 0 (head of chain) */
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify the device */
    vrng_select_queue(q->queue_idx);
    vrng_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->queue_idx);

    /* Busy-wait for completion */
    uint32_t timeout = 100000;
    while (used->idx == prev_used && timeout--) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-RNG] timeout waiting for entropy\n");
        return -1;
    }

    /* The device wrote the random data; return how many bytes it wrote */
    uint16_t used_idx_ent = (prev_used) & (VRING_SIZE - 1);
    uint32_t actual_len = used->ring[used_idx_ent].len;
    return (int)actual_len;
}

/* ── Entropy collection (virtqueue-based) ──────────────────────── */

static void virtio_rng_collect(void)
{
    if (!rng_queue.initialized)
        return;

    /* Request 64 bytes of fresh entropy from the device */
    int ret = vrng_read_entropy(rng_queue.entropy_buf,
                                 sizeof(rng_queue.entropy_buf));
    if (ret > 0) {
        rng_add_entropy(rng_queue.entropy_buf, (uint32_t)ret);
    }
}

/* ── Periodic re-seed helper (called from timer or RNG starvation) ── */

static void virtio_rng_reseed(void)
{
    virtio_rng_collect();
}

/* ── Init ──────────────────────────────────────────────────────── */

static void virtio_rng_init(void)
{
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_RNG_DEVICE, &dev) < 0)
        return;

    rng_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!rng_iobase) return;

    pci_enable_bus_master(&dev);

    /* Reset + acknowledge + driver */
    vrng_outb(VIRTIO_PCI_STATUS, 0);
    vrng_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    if (virtio_negotiate_features_ex(vrng_inl, vrng_outl, vrng_outb, vrng_inb,
                                     VIRTIO_RNG_F_RNG_EE, 0, NULL,
                                     "virtio-rng") < 0) {
        kprintf("[VIRTIO-RNG] feature negotiation failed\n");
        return;
    }

    /* Initialize the virtqueue */
    if (vrng_init_queue() < 0) {
        kprintf("[VIRTIO-RNG] queue initialisation failed\n");
        return;
    }

    /* Driver OK */
    vrng_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
              VIRTIO_STATUS_DRIVER_OK);

    rng_present = 1;

    kprintf("[VIRTIO-RNG] VirtIO RNG at %02x:%02x.%d, I/O 0x%04x, queue active\n",
            dev.bus, dev.slot, dev.func, rng_iobase);

    /* Feed initial entropy into kernel RNG via the virtqueue */
    virtio_rng_collect();
}

#ifdef MODULE
int __init init_module(void) { virtio_rng_init(); return 0; }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO entropy — fill kernel RNG via virtqueue");
MODULE_VERSION("1.0");
#endif

/* ── Stub: virtio_rng_read ─────────────────────────────── */
static int virtio_rng_read(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[VIRTIO] virtio_rng_read: not yet implemented\n");
    return 0;
}
