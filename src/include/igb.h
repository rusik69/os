#ifndef IGB_H
#define IGB_H

#include "types.h"
#include "pci.h"
#include "netdevice.h"
#include "idt.h"

/*
 * src/include/igb.h — Intel Gigabit Ethernet (igb) driver header
 *
 * Supports the Intel 82576 Gigabit Ethernet Controller and I350.
 * These devices provide up to 4 independent TX/RX queues with
 * hardware RSS, MSI-X, and advanced interrupt moderation.
 *
 * Register layout is compatible with the 82576 extended register
 * set (compared to the 82540EM used by the e1000 driver).
 */

/* ── PCI identifiers —──────────────────────────────────────────────── */
#define IGB_VENDOR      0x8086  /* Intel */
#define IGB_DEV_82576   0x10C9  /* 82576 Gigabit (up to 4 queues) */
#define IGB_DEV_I350    0x1521  /* I350 Gigabit (up to 8 queues) */
#define IGB_DEV_I210    0x1533  /* I210 Gigabit (up to 4 queues) */

/* ── Maximum queue configuration —──────────────────────────────────── */
#define IGB_MAX_TX_QUEUES  4
#define IGB_MAX_RX_QUEUES  4

/* ── Descriptor ring sizes —────────────────────────────────────────── */
#define IGB_TX_RING_SIZE     256
#define IGB_RX_RING_SIZE     256
#define IGB_TX_COMP_SIZE     256   /* same ring used for completions */
#define IGB_RX_BUF_SIZE      2048  /* per-RX-buffer size */

/* ── MMIO registers (shared with e1000 for legacy, extended for igb) ─ */

/* Device control */
#define IGB_REG_CTRL      0x0000
#define IGB_REG_STATUS    0x0008
#define IGB_REG_EERD      0x0014

/* Interrupt */
#define IGB_REG_ICR       0x00C0
#define IGB_REG_IMS       0x00D0
#define IGB_REG_IMC       0x00D8
#define IGB_REG_ITR       0x00C4  /* global Interrupt Throttling Rate */

#define IGB_REG_EICR      0x01580 /* Extended ICR (per-vector cause) */
#define IGB_REG_EIMS      0x01588 /* Extended IMS */
#define IGB_REG_EIMC      0x01590 /* Extended IMC */
#define IGB_REG_EIAC      0x0159C /* Extended IAC (auto-clear) */

/* Interrupt Vector Allocation Register (IVAR) — maps queues to vectors */
#define IGB_REG_IVAR0     0x01700 /* Queue 0-3 mapping */
#define IGB_REG_IVAR1     0x01704 /* Queue 4-7 mapping */

/* Per-queue EITR (Extended Interrupt Throttling Rate) */
#define IGB_REG_EITR(q)   (0x01680 + (q) * 4)

/* MAC */
#define IGB_REG_RAL(n)    (0x5400 + (n) * 8)
#define IGB_REG_RAH(n)    (0x5404 + (n) * 8)

/* RX control */
#define IGB_REG_RCTL      0x0100
#define IGB_REG_MRQC      0x5818  /* Multiple Receive Queues Command */
#define IGB_REG_RSSRK(n)  (0x5C80 + (n) * 4)  /* RSS Random Key (4 dwords) */
#define IGB_REG_RETA(i)   (0x5C00 + (i) * 4)  /* RSS Redirection Table */

/* TX control */
#define IGB_REG_TCTL      0x0400

/* ── Per-queue RX register offsets ────────────────────────────────────
 *   { RDBAL, RDBAH, RDLEN, RDH, RDT }
 */
static const uint32_t IGB_RX_Q_REGS[IGB_MAX_RX_QUEUES][5] = {
    {0x2800, 0x2804, 0x2808, 0x2810, 0x2818},  /* queue 0 */
    {0x2C00, 0x2C04, 0x2C08, 0x2C10, 0x2C18},  /* queue 1 */
    {0x3000, 0x3004, 0x3008, 0x3010, 0x3018},  /* queue 2 */
    {0x3400, 0x3404, 0x3408, 0x3410, 0x3418},  /* queue 3 */
};

/* ── Per-queue TX register offsets ────────────────────────────────────
 *   { TDBAL, TDBAH, TDLEN, TDH, TDT }
 */
static const uint32_t IGB_TX_Q_REGS[IGB_MAX_TX_QUEUES][5] = {
    {0x3800, 0x3804, 0x3808, 0x3810, 0x3818},  /* queue 0 */
    {0x3C00, 0x3C04, 0x3C08, 0x3C10, 0x3C18},  /* queue 1 */
    {0x4000, 0x4004, 0x4008, 0x4010, 0x4018},  /* queue 2 */
    {0x4400, 0x4404, 0x4408, 0x4410, 0x4418},  /* queue 3 */
};

/* ── Transmit Descriptor (16 bytes, legacy format) ───────────────────
 * Compatible with e1000/82576 legacy descriptor format.
 */
struct igb_tx_desc {
    uint64_t addr;       /* physical address of packet data */
    uint16_t length;     /* data buffer length */
    uint8_t  cso;        /* checksum offset */
    uint8_t  cmd;        /* descriptor command */
    uint8_t  status;     /* descriptor status */
    uint8_t  css;        /* checksum start */
    uint16_t special;    /* special fields (VLAN, etc.) */
} __attribute__((packed));

/* TX descriptor command bits */
#define IGB_TXD_CMD_EOP     (1U << 0)  /* End of Packet */
#define IGB_TXD_CMD_IFCS    (1U << 1)  /* Insert FCS/CRC */
#define IGB_TXD_CMD_IC      (1U << 2)  /* Insert Checksum */
#define IGB_TXD_CMD_RS      (1U << 3)  /* Report Status */
#define IGB_TXD_CMD_RPS     (1U << 4)  /* Report Packet Sent */
#define IGB_TXD_CMD_DEXT    (1U << 5)  /* Descriptor Extension (0=legacy) */
#define IGB_TXD_CMD_VLE     (1U << 6)  /* VLAN EtherType Insert */
#define IGB_TXD_CMD_IDE     (1U << 7)  /* Interrupt Delay Enable */

/* TX descriptor status bits */
#define IGB_TXD_ST_DD       (1U << 0)  /* Descriptor Done */

/* ── Receive Descriptor (16 bytes, legacy format) ──────────────────── */
struct igb_rx_desc {
    uint64_t addr;       /* physical address of receive buffer */
    uint16_t length;     /* length of received data */
    uint16_t csum;       /* checksum */
    uint8_t  status;     /* descriptor status */
    uint8_t  errors;     /* descriptor errors */
    uint16_t special;    /* special fields (VLAN, RSS hash, etc.) */
} __attribute__((packed));

/* RX descriptor status bits */
#define IGB_RXD_ST_DD       (1U << 0)  /* Descriptor Done */
#define IGB_RXD_ST_EOP      (1U << 1)  /* End of Packet */
#define IGB_RXD_ST_IXSM     (1U << 2)  /* Ignore Checksum Indication */
#define IGB_RXD_ST_VP       (1U << 3)  /* VLAN Present */
#define IGB_RXD_ST_UDPCS    (1U << 4)  /* UDP Checksum */
#define IGB_RXD_ST_TCPCS    (1U << 5)  /* TCP Checksum */
#define IGB_RXD_ST_IPCS     (1U << 6)  /* IP Checksum */
#define IGB_RXD_ST_PIF      (1U << 7)  /* Passed Internal Filter */

/* RX descriptor errors */
#define IGB_RXD_ERR_CE      (1U << 0)  /* CRC Error */
#define IGB_RXD_ERR_SE      (1U << 1)  /* Symbol Error */
#define IGB_RXD_ERR_SEQ     (1U << 2)  /* Sequence Error */
#define IGB_RXD_ERR_CXE     (1U << 3)  /* Carrier Extension Error */
#define IGB_RXD_ERR_TCPE    (1U << 4)  /* TCP/UDP Checksum Error */
#define IGB_RXD_ERR_IPE     (1U << 5)  /* IP Checksum Error */
#define IGB_RXD_ERR_RXE     (1U << 6)  /* RX Data Error */

/* ── Advanced TX Descriptor context (used for offloads) ────────────── */
struct igb_adv_tx_desc {
    uint64_t addr;
    uint32_t cmd_type_len;   /* command/type/length */
    uint32_t olinfo_status;  /* offload info/status */
} __attribute__((packed));

/* ── Advanced RX Descriptor (write-back format) ────────────────────── */
struct igb_adv_rx_desc {
    uint64_t addr;          /* physical address of receive buffer */
    uint32_t rss_hash;      /* RSS hash (filed by device) */
    uint16_t length;        /* received data length */
    uint16_t csum;          /* checksum */
    uint8_t  status;        /* descriptor status */
    uint8_t  errors;        /* descriptor errors */
    uint16_t vlan_tag;      /* VLAN tag (if stripped) */
    uint32_t rsvd;
} __attribute__((packed));

/* ── Receive Queue state ──────────────────────────────────────────── */
struct igb_rxq {
    struct igb_rx_desc *descs;   /* virtual addr of RX descriptor ring */
    uint64_t descs_phys;         /* physical addr of RX descriptor ring */
    int size;                    /* number of descriptors in ring */
    int next;                    /* producer index (where driver fills) */
    int cur;                     /* consumer index (where driver reaps) */
    uint8_t *buffers[IGB_RX_RING_SIZE];   /* virtual addrs of RX bufs */
    uint64_t buf_phys[IGB_RX_RING_SIZE];  /* physical addrs of RX bufs */
};

/* ── Transmit Queue state ─────────────────────────────────────────── */
struct igb_txq {
    struct igb_tx_desc *descs;   /* virtual addr of TX descriptor ring */
    uint64_t descs_phys;         /* physical addr of TX descriptor ring */
    int size;                    /* number of descriptors in ring */
    int next;                    /* producer index */
    int clean;                   /* consumer (clean) index */
};

/* ── Driver private data ──────────────────────────────────────────── */
struct igb_priv {
    int             present;      /* device found and initialised */
    uintptr_t       mmio_base;    /* virtual address of BAR0 MMIO */
    uint64_t        mmio_phys;    /* physical address of BAR0 */
    int             irq_line;     /* legacy IRQ line */
    int             ifindex;      /* registered netdevice index */

    /* MAC address */
    uint8_t         mac[6];

    /* Device identification */
    uint16_t        device_id;

    /* Queue configuration */
    int             num_tx_queues;
    int             num_rx_queues;
    struct igb_txq  txq[IGB_MAX_TX_QUEUES];
    struct igb_rxq  rxq[IGB_MAX_RX_QUEUES];

    /* Shared tx/rx buf page pool base for RX buffers */
    uint64_t        buf_pool_phys;
    void           *buf_pool_virt;

    /* Statistics */
    uint64_t        tx_packets;
    uint64_t        rx_packets;
    uint64_t        tx_bytes;
    uint64_t        rx_bytes;
    uint64_t        tx_errors;
    uint64_t        rx_errors;

    /* Netdevice descriptor */
    struct net_device ndev;

    /* ── MSI-X / interrupt state ───────────────────────────────────── */
    int             msix_nvecs;             /* number of MSI-X vectors allocated */
    int             msix_vector_base;       /* base IRQ vector number */
    volatile uint32_t *msix_table;          /* virtual addr of MSI-X table */
    struct pci_interrupt_config int_cfg;    /* interrupt configuration */
};

/* ── Control register bits ────────────────────────────────────────── */
#define IGB_CTRL_SLU        (1U << 6)   /* Set Link Up */
#define IGB_CTRL_RST        (1U << 26)  /* Device Reset */
#define IGB_CTRL_VME        (1U << 19)  /* VLAN Mode Enable */

/* ── Status register bits ─────────────────────────────────────────── */
#define IGB_STATUS_LU       (1U << 1)   /* Link Up */

/* ── RCTL bits ────────────────────────────────────────────────────── */
#define IGB_RCTL_EN         (1U << 1)   /* Receiver Enable */
#define IGB_RCTL_UPE        (1U << 3)   /* Unicast Promiscuous */
#define IGB_RCTL_MPE        (1U << 4)   /* Multicast Promiscuous */
#define IGB_RCTL_BAM        (1U << 15)  /* Broadcast Accept */
#define IGB_RCTL_BSIZE_2048 0           /* buffer size 2048 */
#define IGB_RCTL_SZ_2048    (0U << 16)  /* descriptor buffer size 2048 */
#define IGB_RCTL_SECRC      (1U << 26)  /* Strip CRC */
#define IGB_RCTL_MQ         (1U << 24)  /* Multiple Queues (required for RSS) */
#define IGB_RCTL_DTYPE_NO_SNOOP (0U << 2) /* No Snoop (default) */

/* ── TCTL bits ────────────────────────────────────────────────────── */
#define IGB_TCTL_EN         (1U << 1)   /* Transmitter Enable */
#define IGB_TCTL_PSP        (1U << 3)   /* Pad Short Packets */
#define IGB_TCTL_CT_SHIFT   4           /* Collision Threshold shift */
#define IGB_TCTL_COLD_SHIFT 12          /* Collision Distance shift */
#define IGB_TCTL_CT         (0x0F << IGB_TCTL_CT_SHIFT)
#define IGB_TCTL_COLD       (0x3F << IGB_TCTL_COLD_SHIFT)

/* ── MRQC bits (Multiple Receive Queues Command) ──────────────────── */
#define IGB_MRQC_RSS        0x00000001U /* RSS Enable */
#define IGB_MRQC_RSS_FIELD  0x00000002U /* Enable RSS hashing on fields */
#define IGB_MRQC_RSS_QUEUES_2  0x00010000U /* 2 RSS queues */
#define IGB_MRQC_RSS_QUEUES_4  0x00020000U /* 4 RSS queues */
#define IGB_MRQC_RSS_QUEUES_8  0x00030000U /* 8 RSS queues */

/* ── RSS hash types ───────────────────────────────────────────────── */
#define IGB_RSS_HASH_TCP_IPV4     (1U << 1)
#define IGB_RSS_HASH_IPV4         (1U << 2)
#define IGB_RSS_HASH_TCP_IPV6     (1U << 3)
#define IGB_RSS_HASH_IPV6         (1U << 4)
#define IGB_RSS_HASH_IPV6_EX      (1U << 5)
#define IGB_RSS_HASH_TCP_IPV6_EX  (1U << 6)

/* ── EICR / EIMS / EIMC bit definitions (Extended Interrupt Cause) ──
 *  These bits indicate which queue(s) caused an MSI-X interrupt.
 *  Same layout in EICR (read to get pending causes), EIMS (write 1
 *  to enable), EIMC (write 1 to mask), EIAC (write 1 to enable
 *  auto-clear-on-read).
 */
#define IGB_EICR_TXQ0        (1U << 0)   /* TX queue 0 completion */
#define IGB_EICR_TXQ1        (1U << 1)   /* TX queue 1 completion */
#define IGB_EICR_TXQ2        (1U << 2)   /* TX queue 2 completion */
#define IGB_EICR_TXQ3        (1U << 3)   /* TX queue 3 completion */
#define IGB_EICR_RXQ0        (1U << 4)   /* RX queue 0 descriptor */
#define IGB_EICR_RXQ1        (1U << 5)   /* RX queue 1 descriptor */
#define IGB_EICR_RXQ2        (1U << 6)   /* RX queue 2 descriptor */
#define IGB_EICR_RXQ3        (1U << 7)   /* RX queue 3 descriptor */
#define IGB_EICR_LSC         (1U << 15)  /* Link Status Change */
#define IGB_EICR_RXQ0_TMR    (1U << 20)  /* RX queue 0 timer */
#define IGB_EICR_RXQ1_TMR    (1U << 21)  /* RX queue 1 timer */
#define IGB_EICR_RXQ2_TMR    (1U << 22)  /* RX queue 2 timer */
#define IGB_EICR_RXQ3_TMR    (1U << 23)  /* RX queue 3 timer */

/* Mask of all TX queue interrupt bits */
#define IGB_EICR_TX_ALL      (IGB_EICR_TXQ0 | IGB_EICR_TXQ1 | IGB_EICR_TXQ2 | IGB_EICR_TXQ3)
/* Mask of all RX queue interrupt bits */
#define IGB_EICR_RX_ALL      (IGB_EICR_RXQ0 | IGB_EICR_RXQ1 | IGB_EICR_RXQ2 | IGB_EICR_RXQ3)
/* Mask of all queue-related interrupt bits (TX + RX) */
#define IGB_EICR_Q_ALL       (IGB_EICR_TX_ALL | IGB_EICR_RX_ALL)

/* ── IVAR entry format (per-queue bitfield within IVAR0/IVAR1) ──────
 *  Byte 0: RX queue 0 [7:0]=vector, [15]=valid
 *  Byte 1: TX queue 0 [23:16]=vector, [31]=valid
 *  Byte 2: RX queue 1 ...
 *  A queue's IVAR entry selects one of up to 16 MSI-X vectors.
 *  bit 7 in the byte = valid bit.
 */
#define IGB_IVAR_VALID      (1U << 7)
#define IGB_IVAR_ENTRY(vec) ((uint32_t)(((vec) & 0x7F) | IGB_IVAR_VALID))

/* ── Default RSS key (if nothing better is available) ────────────────
 * Standard Intel RSS testing key.
 */
static const uint32_t IGB_RSS_KEY_DEFAULT[4] = {
    0x6D5A56DA, 0xBFEF9D52, 0x6D5A56DA, 0xBFEF9D52
};

/* ── Driver API ───────────────────────────────────────────────────── */
int  igb_init(void);
void igb_exit(void);

/* PCI probe */ 
int  igb_find_device(struct pci_device *pci_dev);
int  igb_probe(struct igb_priv *priv);

/* Hardware control */
int  igb_reset_hw(struct igb_priv *priv);
int  igb_init_hw(struct igb_priv *priv);
void igb_shutdown_hw(struct igb_priv *priv);

/* Ring setup */
int  igb_setup_tx_ring(struct igb_priv *priv, int q_idx);
int  igb_setup_rx_ring(struct igb_priv *priv, int q_idx);
void igb_teardown_tx_ring(struct igb_priv *priv, int q_idx);
void igb_teardown_rx_ring(struct igb_priv *priv, int q_idx);

/* Multi-queue / RSS */
void igb_rss_configure(struct igb_priv *priv);
void igb_rss_set_key(struct igb_priv *priv, const uint32_t key[4]);
void igb_rss_set_reta(struct igb_priv *priv);
void igb_ivar_configure(struct igb_priv *priv);

/* Data path */
int  igb_transmit_ring(struct igb_priv *priv, int q_idx,
                       const uint8_t *data, uint16_t len);
int  igb_receive_ring(struct igb_priv *priv, int q_idx,
                      uint8_t *buf, uint16_t max_len);

/* Statistics */
void igb_print_stats(struct igb_priv *priv);

/* Interrupt management */
int  igb_setup_interrupts(struct igb_priv *priv, struct pci_device *pci_dev);
void igb_teardown_interrupts(struct igb_priv *priv);
void igb_irq_handler(struct interrupt_frame *frame);

/* Per-queue interrupt rate limiting */
void igb_set_eitr(struct igb_priv *priv, int q_idx, uint32_t rate);

/* MMIO accessors */
static inline uint32_t igb_readl(struct igb_priv *priv, uint32_t reg)
{
    return *(volatile uint32_t *)(priv->mmio_base + reg);
}

static inline void igb_writel(struct igb_priv *priv, uint32_t reg,
                              uint32_t val)
{
    *(volatile uint32_t *)(priv->mmio_base + reg) = val;
}

#define IGB_PAGE_SIZE  4096

/* Number of contiguous pages for RX buffers (256 descriptors × 2048) */
#define IGB_RX_BUF_PAGE_COUNT  ((IGB_RX_RING_SIZE * IGB_RX_BUF_SIZE + IGB_PAGE_SIZE - 1) / IGB_PAGE_SIZE)

#endif /* IGB_H */
