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

    /* Queues (filled by TX/RX ring setup — task 9) */
    int             num_tx_queues;
    int             num_rx_queues;

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

#endif /* VMXNET3_H */
