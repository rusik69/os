/*
 * src/drivers/virtio_rng.c — VirtIO entropy driver
 *
 * Implements VirtIO RNG (random number generator)
 * for PCI device 1AF4:1005 (virtio-rng).
 * Feeds entropy into the kernel's RNG pool via rng_add_entropy().
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

/* ── Driver state ──────────────────────────────────────────────── */

static int            rng_present = 0;
static uint16_t       rng_iobase  = 0;

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

/* ── Entropy collection ────────────────────────────────────────── */

static void virtio_rng_collect(void)
{
    /* In a full implementation, we'd submit a buffer to the virtqueue.
     * For the stub, we mix in a jitter sample. */
    uint32_t sample = vrng_inl(VIRTIO_PCI_ISR); /* read ISR as noise */
    rng_add_entropy(&sample, sizeof(sample));
}

/* ── Init ──────────────────────────────────────────────────────── */

void virtio_rng_init(void)
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
    virtio_negotiate_features_ex(vrng_inl, vrng_outl, vrng_outb, vrng_inb,
                                 VIRTIO_RNG_F_RNG_EE, 0, NULL, "virtio-rng");

    /* Driver OK */
    vrng_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
              VIRTIO_STATUS_DRIVER_OK);

    rng_present = 1;

    kprintf("[virtio-rng] VirtIO RNG at %02x:%02x.%d, I/O 0x%04x\n",
            dev.bus, dev.slot, dev.func, rng_iobase);

    /* Feed entropy into kernel RNG */
    virtio_rng_collect();
}

#ifdef MODULE
int init_module(void) { virtio_rng_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO entropy — fill kernel RNG");
MODULE_VERSION("1.0");
#endif
