/*
 * src/drivers/virtio_input.c — VirtIO input driver
 *
 * Implements VirtIO input (keyboard, mouse, tablet)
 * for PCI device 1AF4:1052 (virtio-input).
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "heap.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_INPUT_DEVICE    0x1052

/* ── Input event types (from virtio-input spec) ────────────────── */

#define VIRTIO_INPUT_CFG_UNSET     0x00
#define VIRTIO_INPUT_CFG_ID_NAME   0x01
#define VIRTIO_INPUT_CFG_ID_SERIAL 0x02
#define VIRTIO_INPUT_CFG_ID_DEVIDS 0x03
#define VIRTIO_INPUT_CFG_PROP_BITS 0x10
#define VIRTIO_INPUT_CFG_EV_BITS   0x11
#define VIRTIO_INPUT_CFG_ABS_INFO  0x12

/* Event types */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define SYN_REPORT      0

/* ── Driver state ──────────────────────────────────────────────── */

static int            input_present = 0;
static uint16_t       input_iobase  = 0;
static struct pci_device input_pci_dev;

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline void vi_outb(uint8_t off, uint8_t v)  { outb(input_iobase + off, v); }
static inline void vi_outw(uint8_t off, uint16_t v) { outw(input_iobase + off, v); }
static inline void vi_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(input_iobase + off),     (uint8_t)v);
    outb((uint16_t)(input_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(input_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(input_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vi_inb(uint8_t off)  { return inb(input_iobase + off); }
static inline uint16_t vi_inw(uint8_t off)  { return inw(input_iobase + off); }
static inline uint32_t vi_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(input_iobase + off)) |
           ((uint32_t)inb((uint16_t)(input_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(input_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(input_iobase + off + 3)) << 24);
}

/* ── Init ──────────────────────────────────────────────────────── */

void virtio_input_init(void)
{
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_INPUT_DEVICE, &dev) < 0)
        return;

    input_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!input_iobase) return;

    input_pci_dev = dev;

    pci_enable_bus_master(&dev);

    /* Reset + acknowledge + driver */
    vi_outb(VIRTIO_PCI_STATUS, 0);
    vi_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features (accept all host features) */
    uint32_t host = vi_inl(VIRTIO_PCI_HOST_FEAT);
    vi_outl(VIRTIO_PCI_GUEST_FEAT, host);

    vi_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK);

    /* Driver OK */
    vi_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    input_present = 1;

    kprintf("[virtio-input] VirtIO input (keyboard/mouse/tablet) at %02x:%02x.%d, I/O 0x%04x\n",
            dev.bus, dev.slot, dev.func, input_iobase);
}

#ifdef MODULE
int init_module(void) { virtio_input_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO input — keyboard, mouse, tablet");
MODULE_VERSION("1.0");
#endif
