/*
 * virtio-net driver  (legacy PCI device 1AF4:1000)
 *
 * In the standard QEMU x86 machine the default NIC is an e1000, so this
 * driver will normally NOT find a device and virtio_net_init() will return -1.
 * It is included so that kernels running on virtio-net QEMU machines (launched
 * with -device virtio-net-pci) can use the faster paravirtual NIC.
 *
 * Supported: legacy virtio (PCI revision 0, device 1).  Modern (PCIe) virtio
 * is not covered here.
 */

#include "virtio_net.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "types.h"

/* ── Virtio PCI constants ───────────────────────────────────────── */
#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_NET_DEVICE      0x1000

#define VIRTIO_PCI_HOST_FEAT   0x00  /* R   device feature bits    */
#define VIRTIO_PCI_GUEST_FEAT  0x04  /* RW  driver feature bits    */
#define VIRTIO_PCI_QUEUE_PFN   0x08  /* RW  queue phys page number */
#define VIRTIO_PCI_QUEUE_SIZE  0x0C  /* R   queue size             */
#define VIRTIO_PCI_QUEUE_SEL   0x0E  /* RW  queue selector         */
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10 /* W   queue notify           */
#define VIRTIO_PCI_STATUS      0x12  /* RW  device status          */
#define VIRTIO_PCI_ISR         0x13  /* R   ISR status             */

/* Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

/* Virtqueue size (must be power of 2) */
#define VRING_SIZE 16

/* Descriptor flags */
#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

/* ── Virtqueue structures ────────────────────────────────────────── */
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

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VRING_SIZE];
};
#pragma pack(pop)

/* virtio-net header prepended to every packet */
#pragma pack(push, 1)
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  /* only if VIRTIO_NET_F_MRG_RXBUF */
};
#pragma pack(pop)

/* ── Driver state ────────────────────────────────────────────────── */
static int      vnet_present = 0;
static uint16_t vnet_iobase  = 0;

/* TX virtqueue (queue 1) — statically allocated, 4KB aligned */
#define TX_QUEUE_IDX 1
static uint8_t  __attribute__((aligned(4096))) tx_queue_mem[4096];

/* ── Helpers ─────────────────────────────────────────────────────── */
static inline void vio_outb(uint8_t off, uint8_t v)  { outb(vnet_iobase + off, v); }
static inline void vio_outw(uint8_t off, uint16_t v) { outw(vnet_iobase + off, v); }
static inline void vio_outl(uint8_t off, uint32_t v) {
    outb(vnet_iobase + off,     (uint8_t)(v));
    outb(vnet_iobase + off + 1, (uint8_t)(v >> 8));
    outb(vnet_iobase + off + 2, (uint8_t)(v >> 16));
    outb(vnet_iobase + off + 3, (uint8_t)(v >> 24));
}
static inline uint8_t  vio_inb(uint8_t off)  { return inb(vnet_iobase + off); }
static inline uint16_t vio_inw(uint8_t off)  { return inw(vnet_iobase + off); }

/* ── Init ────────────────────────────────────────────────────────── */
int virtio_net_init(void) {
    struct pci_device dev;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_DEVICE, &dev) < 0)
        return -1;

    /* BAR0 is the legacy I/O BAR */
    vnet_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!vnet_iobase) return -1;

    pci_enable_bus_master(&dev);

    /* Reset device */
    vio_outb(VIRTIO_PCI_STATUS, 0);
    /* Acknowledge & driver */
    vio_outb(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    /* Accept all features (we don't negotiate selectively) */
    vio_outl(VIRTIO_PCI_GUEST_FEAT, vio_inb(VIRTIO_PCI_HOST_FEAT));

    /* Set up TX queue (index 1) */
    vio_outw(VIRTIO_PCI_QUEUE_SEL, TX_QUEUE_IDX);
    uint16_t qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
    if (qsz == 0) qsz = VRING_SIZE;
    /* Write queue PFN (4096-byte pages) */
    uint64_t phys = (uint64_t)(uintptr_t)tx_queue_mem;
    vio_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

    /* Driver OK */
    vio_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    vnet_present = 1;
    kprintf("virtio-net: initialized (iobase=0x%x)\n", (uint64_t)vnet_iobase);
    return 0;
}

/* ── Send ────────────────────────────────────────────────────────── */
void virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!vnet_present) return;

    /* Build descriptor ring in tx_queue_mem */
    struct vring_desc  *descs = (struct vring_desc  *)tx_queue_mem;
    struct vring_avail *avail = (struct vring_avail *)(tx_queue_mem +
                                 sizeof(struct vring_desc) * VRING_SIZE);

    /* Descriptor 0: virtio-net header */
    static struct virtio_net_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    descs[0].addr  = (uint64_t)(uintptr_t)&hdr;
    descs[0].len   = sizeof(hdr);
    descs[0].flags = VRING_DESC_F_NEXT;
    descs[0].next  = 1;
    /* Descriptor 1: packet payload */
    descs[1].addr  = (uint64_t)(uintptr_t)data;
    descs[1].len   = len;
    descs[1].flags = 0;
    descs[1].next  = 0;

    /* Add to available ring */
    uint16_t idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[idx] = 0;          /* descriptor chain starts at 0 */
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, TX_QUEUE_IDX);
}

int virtio_net_present(void) { return vnet_present; }
