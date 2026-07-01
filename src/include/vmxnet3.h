#ifndef VMXNET3_H
#define VMXNET3_H

#include "types.h"
#include "pci.h"
#include "netdevice.h"

/* ── PCI identifiers ──────────────────────────────────────────────── */
#define VMXNET3_VENDOR  0x15AD   /* VMware */
#define VMXNET3_DEVICE  0x07B0   /* vmxnet3 */

/* ── MMIO Register Map (BAR0) ───────────────────────────────────────
 *
 * The vmxnet3 adapter exposes a set of 32-bit MMIO registers via BAR0.
 * All reads/writes must be 32-bit wide.
 */
#define VMXNET3_REG_VR0    0x00   /* Version Report 0 (magic) */
#define VMXNET3_REG_VR1    0x04   /* Version Report 1 (magic) */
#define VMXNET3_REG_DSAL   0x08   /* Driver Shared Address (low 32 bits) */
#define VMXNET3_REG_DSAH   0x0C   /* Driver Shared Address (high 32 bits) */
#define VMXNET3_REG_CMD    0x10   /* Command register */
#define VMXNET3_REG_MACL   0x14   /* MAC Address (low 32 bits) */
#define VMXNET3_REG_MACH   0x18   /* MAC Address (high 32 bits) */
#define VMXNET3_REG_ICR    0x1C   /* Interrupt Cause Register */
#define VMXNET3_REG_ECR    0x20   /* Event Cause Register */
#define VMXNET3_REG_IMR    0x28   /* Interrupt Mask Register */

/* ── Magic version values ─────────────────────────────────────────── */
#define VMXNET3_MAGIC_VR0  0x00010000U  /* Version 1 */
#define VMXNET3_MAGIC_VR1  0x00010001U  /* Version 1, sub-variant */

/* ── Command codes ────────────────────────────────────────────────── */
#define VMXNET3_CMD_ACTIVATE       1   /* Activate device */
#define VMXNET3_CMD_DEACTIVATE     2   /* Deactivate device */
#define VMXNET3_CMD_RESET          3   /* Reset device */
#define VMXNET3_CMD_QUIESCE        4   /* Quiesce device */
#define VMXNET3_CMD_SET_FILTER     5   /* Set MAC filter */
#define VMXNET3_CMD_SET_RXMODE     6   /* Set RX mode */
#define VMXNET3_CMD_GET_MAC        7   /* Read MAC address */
#define VMXNET3_CMD_STATS          8   /* Statistics */
#define VMXNET3_CMD_SET_RSS        9   /* Set RSS config */

/* ── Interrupt Cause Register bits ────────────────────────────────── */
#define VMXNET3_ICR_TXDONE0   (1U << 0)  /* TX completion queue 0 */
#define VMXNET3_ICR_RXDONE0   (1U << 1)  /* RX completion queue 0 */
#define VMXNET3_ICR_TXDONE1   (1U << 2)  /* TX completion queue 1 */
#define VMXNET3_ICR_RXDONE1   (1U << 3)  /* RX completion queue 1 */

/* ── Maximum number of TX/RX queues supported ─────────────────────── */
#define VMXNET3_MAX_TX_QUEUES  8
#define VMXNET3_MAX_RX_QUEUES  8

/* ── Descriptor ring sizes ────────────────────────────────────────── */
#define VMXNET3_TX_RING_SIZE     64   /* TX descriptor ring entries */
#define VMXNET3_RX_RING_SIZE     64   /* RX descriptor ring entries */
#define VMXNET3_TX_COMP_SIZE     64   /* TX completion ring entries */
#define VMXNET3_RX_COMP_SIZE     64   /* RX completion ring entries */
#define VMXNET3_RX_BUF_SIZE      2048 /* Per-RX-buffer size (MTU + headers) */

/* ── Shared memory allocation ───────────────────────────────────────
 * All rings and the driver-shared struct live in one contiguous
 * physical region.  Layout within allocated pages:
 *
 *   0x0000  – struct vmxnet3_driver_shared  (padded to 1 page)
 *   0x1000  – TX descriptor ring            (64 × 16 = 1 KB)
 *   0x1400  – TX completion ring            (64 × 16 = 1 KB)
 *   0x1800  – RX descriptor ring            (64 × 16 = 1 KB)
 *   0x1C00  – RX completion ring            (64 × 16 = 1 KB)
 *   0x2000  – RX data buffers               (64 × 2048 = 128 KB)
 *   Total:  ~133 KB, allocate 64 pages (order 6 = 256 KB).
 */
#define VMXNET3_SHARED_PAGES     64   /* 256 KB total shared region */
#define VMXNET3_SHARED_SIZE      (VMXNET3_SHARED_PAGES * 4096ULL)

/* ── Offsets within the shared region ─────────────────────────────── */
#define VMXNET3_TX_RING_OFFSET   0x1000ULL
#define VMXNET3_TX_COMP_OFFSET   0x1400ULL
#define VMXNET3_RX_RING_OFFSET   0x1800ULL
#define VMXNET3_RX_COMP_OFFSET   0x1C00ULL
#define VMXNET3_RX_BUF_OFFSET    0x2000ULL

/* ── Descriptor structures ────────────────────────────────────────── */

/* TX descriptor (16 bytes, packed).
 *
 *   addr: physical address of the packet data to send.
 *   len:  number of bytes to send from addr.
 *   gen:  generation bit – toggled each ring wrap.
 *   eop:  1 = end of packet (last descriptor of a multi-fragment frame).
 *   cq:   1 = generate a completion-entry when the device finishes.
 *   ext:  1 = this descriptor is an "extension" descriptor (ignored here).
 */
struct vmxnet3_tx_desc {
    uint64_t addr;
    uint32_t len;
    uint32_t rsvd1:1;
    uint32_t gen:1;
    uint32_t rsvd2:7;
    uint32_t eop:1;
    uint32_t rsvd3:1;
    uint32_t cq:1;
    uint32_t rsvd4:2;
    uint32_t ext:1;
    uint32_t rsvd5:17;
    uint32_t rsvd6;
    uint64_t rsvd7;
} __attribute__((packed));

/* TX completion entry (16 bytes, packed).
 *
 *   gen: toggled by the device when it fills this entry.
 */
struct vmxnet3_tx_comp {
    uint32_t rsvd1:2;
    uint32_t gen:1;
    uint32_t rsvd2:29;
    uint32_t rsvd3[3];
} __attribute__((packed));

/* RX descriptor (16 bytes, packed).
 *
 *   addr:  physical address of the buffer to receive data into.
 *   len:   buffer length (max bytes the device may write).
 *   gen:   generation bit – toggled each ring wrap.
 *   btype: buffer type – 0 = header buf, 1 = body buf.
 */
struct vmxnet3_rx_desc {
    uint64_t addr;
    uint32_t len;
    uint32_t rsvd1:1;
    uint32_t gen:1;
    uint32_t rsvd2:14;
    uint32_t btype:1;
    uint32_t rsvd3:15;
    uint32_t rsvd4;
    uint64_t rsvd5;
} __attribute__((packed));

/* RX completion entry (16 bytes, packed).
 *
 *   gen: toggled by the device when it fills this entry.
 *   sop: 1 = start of packet.
 *   eop: 1 = end of packet.
 *   len: number of bytes received in this buffer.
 */
struct vmxnet3_rx_comp {
    uint32_t rsvd1:2;
    uint32_t gen:1;
    uint32_t rsvd2:13;
    uint32_t eop:1;
    uint32_t sop:1;
    uint32_t rsvd3:14;
    uint32_t len;
    uint32_t rsvd4[2];
} __attribute__((packed));

/* ── Driver-shared area (at base of shared memory region) ────────────
 *
 * This structure tells the vmxnet3 device where all descriptor rings
 * are located and how big they are.  It is written by the driver and
 * read by the device's firmware.
 */
struct vmxnet3_driver_shared {
    uint32_t tx_num_queues;
    uint32_t rx_num_queues;
    uint64_t tx_ring_phys[VMXNET3_MAX_TX_QUEUES];
    uint64_t rx_ring_phys[VMXNET3_MAX_RX_QUEUES];
    uint64_t tx_comp_phys[VMXNET3_MAX_TX_QUEUES];
    uint64_t rx_comp_phys[VMXNET3_MAX_RX_QUEUES];
    uint32_t tx_ring_size;
    uint32_t rx_ring_size;
    uint32_t tx_comp_size;
    uint32_t rx_comp_size;
    uint32_t rsvd[24];
} __attribute__((packed));

/* ── Per-queue state ───────────────────────────────────────────────── */

struct vmxnet3_txq {
    struct vmxnet3_tx_desc *descs;   /* virtual addr of TX descriptor ring */
    struct vmxnet3_tx_comp *comp;    /* virtual addr of TX completion ring */
    uint64_t descs_phys;             /* physical addr of TX descriptor ring */
    uint64_t comp_phys;              /* physical addr of TX completion ring */
    int size;                        /* number of entries */
    int gen;                         /* current TX descriptor generation */
    int comp_gen;                    /* expected TX completion generation */
    int next;                        /* producer index into TX desc ring */
    int comp_next;                   /* consumer index into TX comp ring */
};

struct vmxnet3_rxq {
    struct vmxnet3_rx_desc *descs;   /* virtual addr of RX descriptor ring */
    struct vmxnet3_rx_comp *comp;    /* virtual addr of RX completion ring */
    uint64_t descs_phys;             /* physical addr of RX descriptor ring */
    uint64_t comp_phys;              /* physical addr of RX completion ring */
    int size;                        /* number of entries */
    int gen;                         /* current RX descriptor generation */
    int comp_gen;                    /* expected RX completion generation */
    int next;                        /* producer (fill) index into RX desc ring */
    int comp_next;                   /* consumer index into RX comp ring */
    uint8_t *buffers[VMXNET3_RX_RING_SIZE];  /* virtual addrs of RX data bufs */
    uint64_t buf_phys[VMXNET3_RX_RING_SIZE]; /* physical addrs of RX data bufs */
};

/* ── Driver private data structure ────────────────────────────────── */
struct vmxnet3_priv {
    int             present;       /* Device found and initialized */
    uintptr_t       mmio_base;     /* Virtual address of BAR0 MMIO */
    uint64_t        mmio_phys;     /* Physical address of BAR0 */
    int             irq_line;      /* IRQ line */
    int             ifindex;       /* Registered interface index */
    struct net_device ndev;        /* Net device descriptor */

    /* MAC address */
    uint8_t         mac[6];

    /* Shared memory region (physically contiguous) */
    uint64_t        shared_phys;   /* physical address of shared region */
    void           *shared_virt;   /* virtual address of shared region */
    int             shared_pages;  /* number of pages in shared region */

    /* Queues */
    int             num_tx_queues;
    int             num_rx_queues;
    struct vmxnet3_txq txq[VMXNET3_MAX_TX_QUEUES];
    struct vmxnet3_rxq rxq[VMXNET3_MAX_RX_QUEUES];

    /* Statistics */
    uint64_t        tx_packets;
    uint64_t        rx_packets;
    uint64_t        tx_bytes;
    uint64_t        rx_bytes;
    uint64_t        tx_errors;
    uint64_t        rx_errors;
};

/* ── MMIO accessors ───────────────────────────────────────────────── */

static inline uint32_t vmxnet3_readl(struct vmxnet3_priv *priv,
                                     uint32_t reg)
{
    return *(volatile uint32_t *)(priv->mmio_base + reg);
}

static inline void vmxnet3_writel(struct vmxnet3_priv *priv,
                                  uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(priv->mmio_base + reg) = val;
}

/* ── Driver API ───────────────────────────────────────────────────── */

int vmxnet3_init(void);
void vmxnet3_exit(void);

/* Ring operations */
int vmxnet3_setup_rings(struct vmxnet3_priv *priv);
void vmxnet3_teardown_rings(struct vmxnet3_priv *priv);
int vmxnet3_activate(struct vmxnet3_priv *priv);
void vmxnet3_deactivate(struct vmxnet3_priv *priv);

/* Data path */
int vmxnet3_transmit(struct vmxnet3_priv *priv,
                     const uint8_t *data, uint16_t len);
int vmxnet3_receive(struct vmxnet3_priv *priv,
                    uint8_t *buf, uint16_t max_len);

#endif /* VMXNET3_H */
