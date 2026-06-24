/*
 * src/drivers/ivshmem.c — QEMU inter-VM shared memory driver
 *
 * Implements a driver for the QEMU ivshmem device (PCI vendor
 * 1AF4, device 1110).  Provides inter-VM shared memory via PCI
 * BAR, with interrupt signalling for peer notifications.
 * Follows existing PCI probe patterns.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define IVSHMEM_VENDOR          0x1AF4
#define IVSHMEM_DEVICE          0x1110

/* ── ivshmem PCI registers (BAR0 — I/O registers) ──────────────── */

#define IVSHMEM_REG_VERSION     0x00
#define IVSHMEM_REG_ID          0x04
#define IVSHMEM_REG_MAX_PEERS   0x08
#define IVSHMEM_REG_INTRMASK    0x0C
#define IVSHMEM_REG_INTRSTATUS  0x10
#define IVSHMEM_REG_DOORBELL    0x14
#define IVSHMEM_REG_IVPOSITION  0x18

/* ── Driver state ──────────────────────────────────────────────── */

static int            ivshmem_present = 0;
static uint16_t       ivshmem_iobase  = 0;
static uint64_t       ivshmem_shmem_phys = 0;
static uint64_t       ivshmem_shmem_size = 0;
static uint32_t       ivshmem_version = 0;
static uint32_t       ivshmem_peer_id = 0;
static uint32_t       ivshmem_max_peers = 0;
static uint32_t       ivshmem_position = 0;

/* ── I/O helpers ───────────────────────────────────────────────── */

static inline uint32_t iv_readl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(ivshmem_iobase + off)) |
           ((uint32_t)inb((uint16_t)(ivshmem_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(ivshmem_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(ivshmem_iobase + off + 3)) << 24);
}

static inline void iv_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(ivshmem_iobase + off),     (uint8_t)v);
    outb((uint16_t)(ivshmem_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(ivshmem_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(ivshmem_iobase + off + 3), (uint8_t)(v >> 24));
}

/* ── Send doorbell to peer ─────────────────────────────────────── */

void ivshmem_ring_doorbell(uint16_t peer, uint16_t vector)
{
    if (!ivshmem_present) return;
    uint32_t bell = ((uint32_t)peer << 16) | vector;
    iv_outl(IVSHMEM_REG_DOORBELL, bell);
}

/* ── Probe ─────────────────────────────────────────────────────── */

void ivshmem_init(void)
{
    struct pci_device dev;
    if (pci_find_device(IVSHMEM_VENDOR, IVSHMEM_DEVICE, &dev) < 0)
        return;

    ivshmem_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!ivshmem_iobase) return;

    /* BAR2: shared memory (64-bit) */
    ivshmem_shmem_phys = dev.bar[2] & ~0xF;
    ivshmem_shmem_size = 0x400000; /* 4MB default */

    pci_enable_bus_master(&dev);

    /* Read registers */
    ivshmem_version   = iv_readl(IVSHMEM_REG_VERSION);
    ivshmem_peer_id   = iv_readl(IVSHMEM_REG_ID);
    ivshmem_max_peers = iv_readl(IVSHMEM_REG_MAX_PEERS);
    ivshmem_position  = iv_readl(IVSHMEM_REG_IVPOSITION);

    ivshmem_present = 1;

    kprintf("[IVSHMEM] QEMU ivshmem at %02x:%02x.%d, I/O 0x%04x, "
            "version=%u, id=%u, peers=%u, position=%u, "
            "shmem phys=0x%llx size=%llu\n",
            dev.bus, dev.slot, dev.func, ivshmem_iobase,
            ivshmem_version, ivshmem_peer_id,
            ivshmem_max_peers, ivshmem_position,
            (unsigned long long)ivshmem_shmem_phys,
            (unsigned long long)ivshmem_shmem_size);
}

#ifdef MODULE
int init_module(void) { ivshmem_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("QEMU inter-VM shared memory — PCI BAR, interrupt");
MODULE_VERSION("1.0");
#endif

/* ── Stub: ivshmem_read ─────────────────────────────── */
int ivshmem_read(void *dev, uint64_t offset, void *buf, size_t count)
{
    (void)dev;
    (void)offset;
    (void)buf;
    (void)count;
    kprintf("[IVSHMEM] ivshmem_read: not yet implemented\n");
    return 0;
}
/* ── Stub: ivshmem_write ─────────────────────────────── */
int ivshmem_write(void *dev, uint64_t offset, const void *buf, size_t count)
{
    (void)dev;
    (void)offset;
    (void)buf;
    (void)count;
    kprintf("[IVSHMEM] ivshmem_write: not yet implemented\n");
    return 0;
}
