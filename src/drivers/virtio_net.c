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
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "net.h"

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
#define VIRTIO_PCI_CONFIG      20    /* device-specific config     */

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

#define RX_QUEUE_IDX 0
#define TX_QUEUE_IDX 1
static uint8_t  __attribute__((aligned(4096))) rx_queue_mem[4096];
static uint8_t  __attribute__((aligned(4096))) tx_queue_mem[4096];
static uint8_t  rx_pkt_bufs[VRING_SIZE][2048];
static uint16_t rx_last_used = 0;
static uint8_t  vnet_irq = 0;
static uint8_t  tx_pkt_buf[2048];
static struct virtio_net_hdr tx_hdr;
static uint16_t tx_last_used = 0;

static inline void vio_outb(uint8_t off, uint8_t v);
static inline void vio_outw(uint8_t off, uint16_t v);
static inline uint8_t vio_inb(uint8_t off);

static void virtio_net_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    (void)vio_inb(VIRTIO_PCI_ISR);
    if (vnet_irq) irq_ack(vnet_irq);
    net_rx_signal();
}

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
static inline uint32_t vio_inl(uint8_t off) {
    return (uint32_t)inb(vnet_iobase + off)
         | ((uint32_t)inb(vnet_iobase + off + 1) << 8)
         | ((uint32_t)inb(vnet_iobase + off + 2) << 16)
         | ((uint32_t)inb(vnet_iobase + off + 3) << 24);
}

static struct vring_avail *vring_avail_ptr(void *base) {
    return (struct vring_avail *)((uint8_t *)base +
                                  sizeof(struct vring_desc) * VRING_SIZE);
}

static struct vring_used *vring_used_ptr(void *base) {
    size_t off = sizeof(struct vring_desc) * VRING_SIZE;
    off += sizeof(uint16_t) * 2 + (size_t)VRING_SIZE * sizeof(uint16_t);
    off = (off + 3) & ~3u;
    return (struct vring_used *)((uint8_t *)base + off);
}

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
    /* Accept host features except mergeable RX buffers (simpler header layout) */
    uint32_t host_feat = vio_inl(VIRTIO_PCI_HOST_FEAT);
    host_feat &= ~(1u << 15);
    vio_outl(VIRTIO_PCI_GUEST_FEAT, host_feat);

    /* RX queue (0): populate ring memory before publishing PFN */
    vio_outw(VIRTIO_PCI_QUEUE_SEL, RX_QUEUE_IDX);
    {
        uint16_t qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (qsz != 0 && qsz < VRING_SIZE) {
            kprintf("virtio-net: queue size %u < %u\n", (uint64_t)qsz, (uint64_t)VRING_SIZE);
            return -1;
        }
        struct vring_desc  *descs = (struct vring_desc  *)rx_queue_mem;
        struct vring_avail *avail = vring_avail_ptr(rx_queue_mem);
        struct vring_used  *used  = vring_used_ptr(rx_queue_mem);
        avail->flags = 0;
        avail->idx = 0;
        used->flags = 0;
        used->idx = 0;
        for (int i = 0; i < VRING_SIZE; i++) {
            descs[i].addr  = VIRT_TO_PHYS(rx_pkt_bufs[i]);
            descs[i].len   = sizeof(rx_pkt_bufs[0]);
            descs[i].flags = VRING_DESC_F_WRITE;
            descs[i].next  = 0;
            uint16_t slot = avail->idx & (VRING_SIZE - 1);
            avail->ring[slot] = (uint16_t)i;
            avail->idx++;
        }
        rx_last_used = 0;
    }
    vio_outl(VIRTIO_PCI_QUEUE_PFN,
             (uint32_t)(VIRT_TO_PHYS(rx_queue_mem) >> 12));

    /* TX queue (1) */
    vio_outw(VIRTIO_PCI_QUEUE_SEL, TX_QUEUE_IDX);
    {
        uint16_t qsz = vio_inw(VIRTIO_PCI_QUEUE_SIZE);
        if (qsz != 0 && qsz < VRING_SIZE) return -1;
        struct vring_avail *avail = vring_avail_ptr(tx_queue_mem);
        struct vring_used  *used  = vring_used_ptr(tx_queue_mem);
        avail->flags = 0;
        avail->idx = 0;
        used->flags = 0;
        used->idx = 0;
    }
    vio_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(VIRT_TO_PHYS(tx_queue_mem) >> 12));

    /* Driver OK, then kick RX so the device picks up buffers */
    vio_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    vio_outw(VIRTIO_PCI_QUEUE_SEL, RX_QUEUE_IDX);
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);

    vnet_irq = dev.irq;
    idt_register_handler(32 + dev.irq, virtio_net_irq_handler);
    if (apic_is_init_complete())
        ioapic_unmask_irq(dev.irq);
    pic_unmask(dev.irq);

    vnet_present = 1;
    kprintf("virtio-net: initialized (iobase=0x%x)\n", (uint64_t)vnet_iobase);
    return 0;
}

/* ── Send ────────────────────────────────────────────────────────── */
int virtio_net_send(const uint8_t *data, uint32_t len) {
    if (!vnet_present) return -1;

    struct vring_desc  *descs = (struct vring_desc  *)tx_queue_mem;
    struct vring_avail *avail = vring_avail_ptr(tx_queue_mem);
    struct vring_used  *used  = vring_used_ptr(tx_queue_mem);

    /* Wait for previous TX to complete */
    uint64_t spin = 0;
    while (used->idx == tx_last_used && spin++ < 1000000)
        __asm__ volatile("" ::: "memory");
    if (used->idx == tx_last_used) return -1;

    if (len > sizeof(tx_pkt_buf)) len = sizeof(tx_pkt_buf);
    memcpy(tx_pkt_buf, data, len);

    memset(&tx_hdr, 0, sizeof(tx_hdr));
    descs[0].addr  = VIRT_TO_PHYS(&tx_hdr);
    descs[0].len   = sizeof(tx_hdr);
    descs[0].flags = VRING_DESC_F_NEXT;
    descs[0].next  = 1;
    descs[1].addr  = VIRT_TO_PHYS(tx_pkt_buf);
    descs[1].len   = len;
    descs[1].flags = 0;
    descs[1].next  = 0;

    uint16_t idx = avail->idx & (VRING_SIZE - 1);
    avail->ring[idx] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");

    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, TX_QUEUE_IDX);

    spin = 0;
    while (used->idx == tx_last_used && spin++ < 1000000)
        __asm__ volatile("" ::: "memory");
    if (used->idx == tx_last_used) return -1;
    tx_last_used = used->idx;
    return 0;
}

int virtio_net_receive(void *buf, uint16_t max_len) {
    if (!vnet_present) return -1;

    struct vring_used *used = vring_used_ptr(rx_queue_mem);
    struct vring_avail *avail = vring_avail_ptr(rx_queue_mem);

    if (used->idx == rx_last_used) return 0;

    __asm__ volatile("" ::: "memory");
    uint16_t uidx = rx_last_used & (VRING_SIZE - 1);
    uint32_t id = used->ring[uidx].id;
    uint32_t total = used->ring[uidx].len;
    rx_last_used++;

    uint32_t skip = sizeof(struct virtio_net_hdr);
    if (total <= skip) return 0;
    uint32_t plen = total - skip;
    if (plen > max_len) plen = max_len;
    memcpy(buf, rx_pkt_bufs[id] + skip, plen);

    struct vring_desc *descs = (struct vring_desc *)rx_queue_mem;
    descs[id].addr  = VIRT_TO_PHYS(rx_pkt_bufs[id]);
    descs[id].len   = sizeof(rx_pkt_bufs[0]);
    descs[id].flags = VRING_DESC_F_WRITE;
    uint16_t slot = avail->idx & (VRING_SIZE - 1);
    avail->ring[slot] = (uint16_t)id;
    __asm__ volatile("" ::: "memory");
    avail->idx++;
    __asm__ volatile("" ::: "memory");
    vio_outw(VIRTIO_PCI_QUEUE_NOTIFY, RX_QUEUE_IDX);
    return (int)plen;
}

void virtio_net_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++)
        mac[i] = vio_inb(VIRTIO_PCI_CONFIG + (uint8_t)i);
}

int virtio_net_present(void) { return vnet_present; }
