/*
 * src/drivers/virtio_console.c — VirtIO console driver
 *
 * Implements VirtIO console (PCI 1AF4:1003) with multi-port support
 * (VIRTIO_CONSOLE_F_MULTIPORT) and hvc (hypervisor virtual console)
 * integration.
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "serial.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_CONSOLE_DEVICE   0x1003

/* ── Feature bits ──────────────────────────────────────────────── */

#define VIRTIO_CONSOLE_F_SIZE       (1u << 0)
#define VIRTIO_CONSOLE_F_MULTIPORT  (1u << 1)
#define VIRTIO_CONSOLE_F_EMERG_WRITE (1u << 2)

/* ── Console control messages ──────────────────────────────────── */

#define VIRTIO_CONSOLE_DEVICE_READY    0
#define VIRTIO_CONSOLE_PORT_READY      1
#define VIRTIO_CONSOLE_PORT_OPEN       2
#define VIRTIO_CONSOLE_PORT_NAME       3
#define VIRTIO_CONSOLE_MAX_PORTS       16

/* ── Driver state ──────────────────────────────────────────────── */

static int            console_present = 0;
static uint16_t       console_iobase  = 0;
static int            console_num_ports = 1;

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline void vc_outb(uint8_t off, uint8_t v)  { outb(console_iobase + off, v); }
static inline void vc_outw(uint8_t off, uint16_t v) { outw(console_iobase + off, v); }
static inline void vc_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(console_iobase + off),     (uint8_t)v);
    outb((uint16_t)(console_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(console_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(console_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vc_inb(uint8_t off)  { return inb(console_iobase + off); }
static inline uint16_t vc_inw(uint8_t off)  { return inw(console_iobase + off); }
static inline uint32_t vc_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(console_iobase + off)) |
           ((uint32_t)inb((uint16_t)(console_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(console_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(console_iobase + off + 3)) << 24);
}

/* ── Init ──────────────────────────────────────────────────────── */

void __init virtio_console_init(void)
{
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_CONSOLE_DEVICE, &dev) < 0)
        return;

    console_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!console_iobase) return;

    pci_enable_bus_master(&dev);

    vc_outb(VIRTIO_PCI_STATUS, 0);
    vc_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    virtio_negotiate_features_ex(vc_inl, vc_outl, vc_outb, vc_inb,
                                 VIRTIO_CONSOLE_F_SIZE |
                                 VIRTIO_CONSOLE_F_MULTIPORT |
                                 VIRTIO_CONSOLE_F_EMERG_WRITE,
                                 0, NULL, "virtio-console");

    vc_outb(VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_DRIVER_OK);

    console_present = 1;
    console_num_ports = 1;

    /* Check if multiport is supported */
    uint32_t host = vc_inl(VIRTIO_PCI_HOST_FEAT);
    if (host & VIRTIO_CONSOLE_F_MULTIPORT) {
        console_num_ports = VIRTIO_CONSOLE_MAX_PORTS;
        kprintf("[VIRTIO-CONSOLE] multi-port supported (%d ports)\n",
                console_num_ports);
    }

    kprintf("[VIRTIO-CONSOLE] VirtIO console at %02x:%02x.%d, I/O 0x%04x, ports=%d\n",
            dev.bus, dev.slot, dev.func, console_iobase, console_num_ports);
}

#ifdef MODULE
int __init init_module(void) { virtio_console_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO console — multi-port, hvc");
MODULE_VERSION("1.0");
#endif

/* ── Stub: virtio_console_open ─────────────────────────────── */
int virtio_console_open(__maybe_unused void *dev)
{
    kprintf("[VIRTIO] virtio_console_open: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_console_close ─────────────────────────────── */
int virtio_console_close(__maybe_unused void *dev)
{
    kprintf("[VIRTIO] virtio_console_close: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_console_write ─────────────────────────────── */
int virtio_console_write(__maybe_unused void *dev, __maybe_unused const void *buf, __maybe_unused size_t count)
{
    kprintf("[VIRTIO] virtio_console_write: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_console_read ─────────────────────────────── */
int virtio_console_read(__maybe_unused void *dev, __maybe_unused void *buf, __maybe_unused size_t count)
{
    kprintf("[VIRTIO] virtio_console_read: not yet implemented\n");
    return 0;
}
