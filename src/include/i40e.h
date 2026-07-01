#ifndef I40E_H
#define I40E_H

#include "types.h"
#include "pci.h"
#include "netdevice.h"
#include "idt.h"

/*
 * src/include/i40e.h — Intel XL710/X710 40GbE Controller (i40e) driver header
 *
 * Supports the Intel XL710 (0x1572) 40GbE and X710 (0x1581) 10GbE
 * physical function (PF) devices.  Provides single-queue and multi-queue
 * operation with MSI-X interrupt support.
 *
 * Task 12: i40e: PF driver with MSI-X
 *   - PCI device identification
 *   - MMIO mapping and device reset
 *   - MAC address reading
 *   - Single queue TX/RX descriptor rings
 *   - MSI-X interrupt setup via pci_setup_interrupts
 *   - PFINT_ICR0-based interrupt handling
 *   - Netdevice registration
 */

/* ── PCI identifiers ─────────────────────────────────────────────────── */
#define I40E_VENDOR       0x8086  /* Intel */
#define I40E_DEV_XL710    0x1572  /* XL710 40GbE (PF) */
#define I40E_DEV_X710     0x1581  /* X710 10GbE (PF) */
#define I40E_DEV_XL710_A  0x1580  /* XL710 alternate (PF) */
#define I40E_DEV_VF       0x154C  /* XL710/X710 Virtual Function */
#define I40E_DEV_VF_HV    0x1570  /* XL710 VF for Hyper-V */

/* ── Maximum queue configuration ─────────────────────────────────────── */
#define I40E_MAX_TX_QUEUES   8
#define I40E_MAX_RX_QUEUES   8

/* ── Descriptor ring sizes ──────────────────────────────────────────── */
#define I40E_TX_RING_SIZE     256
#define I40E_RX_RING_SIZE     256
#define I40E_RX_BUF_SIZE      2048
#define I40E_PAGE_SIZE        4096

/* ── MMIO BAR0 size (4 MB for PF) ────────────────────────────────────── */
#define I40E_BAR0_SIZE        0x400000

/* ── General registers ──────────────────────────────────────────────── */
#define I40E_PFGEN_CTRL       0x00B100
#define I40E_GLGEN_RSTCTL     0x00BF00
#define I40E_GLGEN_STATUS     0x00BF10

/* PFGEN_CTRL bits */
#define I40E_PFGEN_CTRL_RESET_MB  (1U << 1)

/* GLGEN_RSTCTL bits */
#define I40E_GLGEN_RSTCTL_PFR  (1U << 0)   /* Port Reset */

/* GLGEN_STATUS bits */
#define I40E_GLGEN_STATUS_PFR_STAT (1U << 2)  /* Port Reset in progress */

/* ── Interrupt registers ──────────────────────────────────────────── */
#define I40E_PFINT_ICR0       0x00E000  /* Interrupt Cause Register 0 */
#define I40E_PFINT_ICR0_ENA   0x00E008  /* ICR0 Interrupt Enable */
#define I40E_PFINT_DYN_CTLN(n) (0x002000 + (n) * 4)  /* Per-vector dynamic ctl */
#define I40E_PFINT_LNKLSTN(n)  (0x002100 + (n) * 4)  /* Per-vector linked list */
#define I40E_PFINT_ITRN(n, vec) (0x002400 + (vec) * 8 + (n) * 0x2000) /* ITR */

/* PFINT_ICR0 bit definitions */
#define I40E_ICR0_INT_M      (1U << 0)   /* Interrupt Asserted */
#define I40E_ICR0_LSC        (1U << 1)   /* Link Status Change */
#define I40E_ICR0_ADMINQ     (1U << 2)   /* Admin Queue Event */
#define I40E_ICR0_MDDET      (1U << 3)   /* Malicious Driver Detect */
#define I40E_ICR0_GRST       (1U << 11)  /* Global Reset */
#define I40E_ICR0_HMC        (1U << 12)  /* HMC Error */
#define I40E_ICR0_PCIEXCEPLY (1U << 13)  /* PCIe Completion Timeout */
#define I40E_ICR0_ECC_ERR    (1U << 16)  /* ECC Error */
#define I40E_ICR0_TX_QUEUE   (1U << 21)  /* TX Queue completion */
#define I40E_ICR0_RX_QUEUE   (1U << 22)  /* RX Queue completion */
#define I40E_ICR0_RESET_DONE (1U << 24)  /* Reset Done */

/* PFINT_DYN_CTLN bits */
#define I40E_PFINT_DYN_CTLN_INTENA_M     (1U << 0)  /* Enable interrupt */
#define I40E_PFINT_DYN_CTLN_SW_ITR_SHIFT 3
#define I40E_PFINT_DYN_CTLN_SW_ITR_MASK  (0x7U << 3)
#define I40E_PFINT_DYN_CTLN_ITR_0        0           /* ITR #0 */
#define I40E_PFINT_DYN_CTLN_ITR_1        (1U << 3)
#define I40E_PFINT_DYN_CTLN_ITR_2        (2U << 3)
#define I40E_PFINT_DYN_CTLN_ITR_NONE     (7U << 3)
#define I40E_PFINT_DYN_CTLN_INTENA       (1U << 31)  /* Write 1 to arm */

/* PFINT_ITRN register indices (n = 0, 1, 2) */
#define I40E_ITR_INDEX_0   0
#define I40E_ITR_INDEX_1   1
#define I40E_ITR_INDEX_2   2

/* ── Queue registers (per queue, stride 0x40 for RX and TX) ────────── */
#define I40E_QRX_BASE(n)          (0x000C000 + (n) * 0x40)
#define I40E_QRX_TAIL(n)          (I40E_QRX_BASE(n) + 0x00)
#define I40E_QRX_HEAD(n)          (I40E_QRX_BASE(n) + 0x04)
#define I40E_QRX_LENGTH(n)        (I40E_QRX_BASE(n) + 0x08)
#define I40E_QRX_BASE_ADDR_L(n)   (I40E_QRX_BASE(n) + 0x10)
#define I40E_QRX_BASE_ADDR_H(n)   (I40E_QRX_BASE(n) + 0x14)
#define I40E_QRX_ENABLED(n)       (I40E_QRX_BASE(n) + 0x1C)

#define I40E_QTX_BASE(n)          (0x0008000 + (n) * 0x40)
#define I40E_QTX_TAIL(n)          (I40E_QTX_BASE(n) + 0x00)
#define I40E_QTX_HEAD(n)          (I40E_QTX_BASE(n) + 0x04)
#define I40E_QTX_LENGTH(n)        (I40E_QTX_BASE(n) + 0x08)
#define I40E_QTX_BASE_ADDR_L(n)   (I40E_QTX_BASE(n) + 0x10)
#define I40E_QTX_BASE_ADDR_H(n)   (I40E_QTX_BASE(n) + 0x14)
#define I40E_QTX_ENABLED(n)       (I40E_QTX_BASE(n) + 0x1C)

/* ── Receive control register ───────────────────────────────────────── */
#define I40E_PRT_CRCSPCTRL      0x000B0C0
#define I40E_PRT_SCTPL          0x000B0C8
#define I40E_PRT_SRCRXE         0x000B0E0
#define I40E_PRT_SRCRXE_ENA     (1U << 0)

/* ── VF-specific registers (within VF's 16KB MMIO window) ───────────── */
#define I40E_VFGEN_RSTAT        0x008800  /* VF reset / status */
#define I40E_VFGEN_RSTAT_VFR_STATE  0x3U     /* VF reset status mask */

/* VF reset status values */
#define I40E_VFR_VFACTIVE         0       /* VF is active (post-reset) */
#define I40E_VFR_COMPLETED        1       /* VF reset has completed */
#define I40E_VFR_INPROGRESS       2       /* VF reset in progress */

/* VF interrupt registers (same offsets as PF for VF's own MMIO) */
#define I40E_VFINT_ICR0        0x000000  /* VF Interrupt Cause 0 */
#define I40E_VFINT_DYN_CTLN(n) (0x002000 + (n) * 4)  /* VF dyn ctl */
#define I40E_VFINT_ICR0_ENA    0x00E008  /* VF ICR0 Interrupt Enable */

/* ── VF-PF Mailbox registers (PF MMIO space, per-VF) ────────────────── */
/* At offset 0x002C00, each VF gets 16 bytes of mailbox registers */
#define I40E_PF_VF_MBX_REGION  0x002C00
#define I40E_VF_MBX_STRIDE     16
/* Each VF mailbox block: */
#define I40E_VF_MBX_CTL(n)     (I40E_PF_VF_MBX_REGION + (n) * I40E_VF_MBX_STRIDE + 0x00)
#define I40E_VF_MBX_DATA0(n)   (I40E_PF_VF_MBX_REGION + (n) * I40E_VF_MBX_STRIDE + 0x04)
#define I40E_VF_MBX_DATA1(n)   (I40E_PF_VF_MBX_REGION + (n) * I40E_VF_MBX_STRIDE + 0x08)
#define I40E_VF_MBX_DATA2(n)   (I40E_PF_VF_MBX_REGION + (n) * I40E_VF_MBX_STRIDE + 0x0C)

/* Mailbox control bits */
#define I40E_MBX_CTL_VF2PF      (1U << 0)  /* VF → PF message pending */
#define I40E_MBX_CTL_PF2VF      (1U << 1)  /* PF → VF message pending */
#define I40E_MBX_CTL_ACK        (1U << 2)  /* Acknowledge (cleared by receiver) */

/* Mailbox message types (written to DATA0) */
#define I40E_MBX_MSG_NONE        0        /* No message */
#define I40E_MBX_MSG_VERSION     1        /* VF version exchange */
#define I40E_MBX_MSG_GET_MAC     2        /* VF requests MAC address */
#define I40E_MBX_MSG_SET_MAC     3        /* VF sets MAC address */
#define I40E_MBX_MSG_LINK_STATUS 4        /* Link status update */
#define I40E_MBX_MSG_RESET       5        /* VF reset request */
#define I40E_MBX_MSG_STATS       6        /* Statistics request */

/* ── MAC address registers ──────────────────────────────────────────── */
#define I40E_PF_HENA(n)         (0x00B010 + (n) * 4)
#define I40E_PRT_PFMAC_L       0x000B0A0
#define I40E_PRT_PFMAC_H       0x000B0A4

/* ── DMA TX control ─────────────────────────────────────────────────── */
#define I40E_PRTDCB_GENC       0x00088400
#define I40E_PRTDCB_GENC_PFRST (1U << 5)   /* Port Flush Request */

/* ── Transmit Descriptor (32 bytes, advanced) ─────────────────────────
 * The i40e uses a 32-byte descriptor format (both legacy and advanced).
 * We use the advanced format for full offload support.
 */
struct i40e_tx_desc {
    uint64_t addr;          /* physical address of packet data */
    uint32_t cmd_type_len;  /* command / type / length */
    uint32_t olinfo_status; /* offload info / completion status */
    uint32_t rsvd;          /* reserved (must be 0) */
    uint32_t bti;           /* buffer type info / FD filter */
} __attribute__((packed));

/* TX descriptor command bits */
#define I40E_TXD_CMD_EOP           (1U << 0)   /* End of Packet */
#define I40E_TXD_CMD_IFCS          (1U << 1)   /* Insert FCS */
#define I40E_TXD_CMD_IC            (1U << 2)   /* Insert Checksum */
#define I40E_TXD_CMD_RS            (1U << 3)   /* Report Status */
#define I40E_TXD_CMD_DEXT          (1U << 5)   /* Descriptor Extension (1=adv) */
#define I40E_TXD_CMD_VLE           (1U << 6)   /* VLAN Insert Enable */
#define I40E_TXD_CMD_ILT           (1U << 7)   /* Interrupt on Last Threshold */

/* cmd_type_len fields — when DEXT=0 (legacy):
 *   cmd_type_len[63:56] = cmd (8 bits)
 *   cmd_type_len[55:48] = type  (0 for legacy)
 *   cmd_type_len[47:36] = reserved (12 bits)
 *   cmd_type_len[35:18] = head WB enable
 *   cmd_type_len[17:4]  = length (14 bits)
 *   cmd_type_len[3:0]   = reserved
 * For advanced (DEXT=1): cmd_type_len[63:56]=cmd, [55:52]=type(1=adv),
 *   [51:48]=reserved, [47:32]=metdata, [31:4]=payload_len.
 * We use the simple cmd+len encoding: (cmd << 24) | (len & 0x3FFFF).
 */
#define I40E_TXD_TYPE_LEGACY       0
#define I40E_TXD_TYPE_ADV          (1U << 4)   /* advanced type field */

/* TX descriptor status bits (from olinfo_status) */
#define I40E_TXD_ST_DD             (1U << 0)   /* Descriptor Done */

/* ── Receive Descriptor (32 bytes, advanced) ───────────────────────── */
struct i40e_rx_desc {
    uint64_t addr;          /* physical address of receive buffer */
    uint64_t rsvd;          /* reserved */
    uint32_t hdr_addr;      /* header address / status field */
    uint32_t hdr_len;       /* header length / RSS hash */
    uint32_t rss_hash;      /* RSS hash value */
    uint32_t status_error;  /* status and error */
    uint32_t length;        /* received data length */
    uint32_t vlan_tag;      /* VLAN tag (if stripped) */
    uint64_t rsvd2;         /* reserved */
} __attribute__((packed));

/* RX descriptor status bits (from status_error) */
#define I40E_RXD_ST_DD            (1U << 0)   /* Descriptor Done */
#define I40E_RXD_ST_EOP           (1U << 1)   /* End of Packet */
#define I40E_RXD_ST_RSVD         (1U << 2)   /* reserved */
#define I40E_RXD_ST_VP           (1U << 3)   /* VLAN Present */
#define I40E_RXD_ST_IPCS         (1U << 4)   /* IP Checksum */
#define I40E_RXD_ST_TCPCS        (1U << 5)   /* TCP Checksum */
#define I40E_RXD_ST_UDPCS        (1U << 6)   /* UDP Checksum */
#define I40E_RXD_ST_PIF          (1U << 7)   /* Passed Internal Filter */

/* RX descriptor error bits */
#define I40E_RXD_ERR_CE           (1U << 0)   /* CRC Error */
#define I40E_RXD_ERR_SE           (1U << 1)   /* Symbol Error */
#define I40E_RXD_ERR_RXE          (1U << 6)   /* RX Data Error */

/* ── Receive Queue state ────────────────────────────────────────────── */
struct i40e_rxq {
    struct i40e_rx_desc *descs;      /* virtual addr of RX descriptor ring */
    uint64_t descs_phys;             /* physical addr of RX descriptor ring */
    int size;                        /* number of descriptors in ring */
    int next;                        /* consumer index (where driver reaps) */
    int cur;                         /* internal producer index tracking */
    uint8_t *buffers[I40E_RX_RING_SIZE];     /* virtual addrs of RX bufs */
    uint64_t buf_phys[I40E_RX_RING_SIZE];    /* physical addrs of RX bufs */
};

/* ── Transmit Queue state ───────────────────────────────────────────── */
struct i40e_txq {
    struct i40e_tx_desc *descs;      /* virtual addr of TX descriptor ring */
    uint64_t descs_phys;             /* physical addr of TX descriptor ring */
    int size;                        /* number of descriptors in ring */
    int next;                        /* producer index */
    int clean;                       /* consumer (clean) index */
};

/* ── Driver private data ────────────────────────────────────────────── */
struct i40e_priv {
    int             present;         /* device found and initialised */
    uintptr_t       mmio_base;       /* virtual address of BAR0 MMIO */
    uint64_t        mmio_phys;       /* physical address of BAR0 */
    int             irq_line;        /* legacy IRQ line */
    int             ifindex;         /* registered netdevice index */

    /* MAC address */
    uint8_t         mac[6];

    /* Device identification */
    uint16_t        device_id;

    /* Queue configuration */
    int             num_tx_queues;
    int             num_rx_queues;
    struct i40e_txq txq[I40E_MAX_TX_QUEUES];
    struct i40e_rxq rxq[I40E_MAX_RX_QUEUES];

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
    int             msix_nvecs;              /* number of MSI-X vectors alloc'd */
    int             msix_vector_base;        /* base IRQ vector number */
    volatile uint32_t *msix_table;           /* virtual addr of MSI-X table */
    struct pci_interrupt_config int_cfg;     /* interrupt configuration */

    /* ── SR-IOV VF state ────────────────────────────────────────── */
    int             num_vfs;                 /* number of enabled VFs */
    int             vf_bus[I40E_MAX_TX_QUEUES];   /* VF PCI bus numbers */
    int             vf_dev[I40E_MAX_TX_QUEUES];   /* VF PCI device numbers */
    uint8_t         vf_macs[I40E_MAX_TX_QUEUES][6]; /* VF MAC addresses */
};

/* ── VF (Virtual Function) private data ───────────────────────────────── */
struct i40e_vf_priv {
    int             present;         /* VF device found and initialised */
    uintptr_t       mmio_base;       /* virtual addr of VF BAR0 MMIO */
    uint64_t        mmio_phys;       /* physical addr of VF BAR0 */
    int             irq_line;        /* legacy IRQ line */
    int             ifindex;         /* registered netdevice index */
    int             vf_number;       /* VF index (0-based, assigned by PF) */

    /* PF information — for mailbox communication */
    int             pf_bus, pf_dev, pf_func;

    /* MAC address (assigned by PF) */
    uint8_t         mac[6];

    /* Device identification */
    uint16_t        device_id;

    /* Queue configuration */
    int             num_tx_queues;
    int             num_rx_queues;
    struct i40e_txq txq[I40E_MAX_TX_QUEUES];
    struct i40e_rxq rxq[I40E_MAX_RX_QUEUES];

    /* Statistics */
    uint64_t        tx_packets, rx_packets;
    uint64_t        tx_bytes, rx_bytes;
    uint64_t        tx_errors, rx_errors;

    /* Netdevice descriptor */
    struct net_device ndev;

    /* Interrupt configuration */
    struct pci_interrupt_config int_cfg;
};

/* ── Mailbox message (VF↔PF) ─────────────────────────────────────────── */
struct i40e_mbx_msg {
    uint32_t type;       /* I40E_MBX_MSG_* */
    uint32_t data0;      /* type-specific payload */
    uint32_t data1;      /* type-specific payload */
    uint32_t data2;      /* type-specific payload */
};

/* ── MMIO accessors ────────────────────────────────────────────────── */
static inline uint32_t i40e_readl(struct i40e_priv *priv, uint32_t reg)
{
    return *(volatile uint32_t *)(priv->mmio_base + reg);
}

static inline void i40e_writel(struct i40e_priv *priv, uint32_t reg,
                               uint32_t val)
{
    *(volatile uint32_t *)(priv->mmio_base + reg) = val;
}

/* ── RX buffer pool page count ──────────────────────────────────────── */
#define I40E_RX_BUF_PAGE_COUNT \
    ((I40E_RX_RING_SIZE * I40E_RX_BUF_SIZE + I40E_PAGE_SIZE - 1) / I40E_PAGE_SIZE)

/* ── Driver API ─────────────────────────────────────────────────────── */
int  i40e_init(void);
void i40e_exit(void);

/* PCI probe */
int  i40e_find_device(struct pci_device *pci_dev);
int  i40e_probe(struct i40e_priv *priv);

/* Hardware control */
int  i40e_reset_hw(struct i40e_priv *priv);
int  i40e_init_hw(struct i40e_priv *priv);
void i40e_shutdown_hw(struct i40e_priv *priv);

/* Ring setup */
int  i40e_setup_tx_ring(struct i40e_priv *priv, int q_idx);
int  i40e_setup_rx_ring(struct i40e_priv *priv, int q_idx);
void i40e_teardown_tx_ring(struct i40e_priv *priv, int q_idx);
void i40e_teardown_rx_ring(struct i40e_priv *priv, int q_idx);

/* Data path */
int  i40e_transmit_ring(struct i40e_priv *priv, int q_idx,
                        const uint8_t *data, uint16_t len);
int  i40e_receive_ring(struct i40e_priv *priv, int q_idx,
                       uint8_t *buf, uint16_t max_len);

/* Interrupt management */
int  i40e_setup_interrupts(struct i40e_priv *priv, struct pci_device *pci_dev);
void i40e_teardown_interrupts(struct i40e_priv *priv);
void i40e_irq_handler(struct interrupt_frame *frame);

/* Statistics */
void i40e_print_stats(struct i40e_priv *priv);

/* ── VF (Virtual Function) driver API ─────────────────────────────────── */
int  i40e_vf_find_device(struct pci_device *pci_dev);
int  i40e_vf_probe(struct i40e_vf_priv *priv);
int  i40e_vf_init_hw(struct i40e_vf_priv *priv);
void i40e_vf_shutdown_hw(struct i40e_vf_priv *priv);
int  i40e_vf_init(void);
void i40e_vf_exit(void);
void i40e_vf_irq_handler(struct interrupt_frame *frame);

/* ── SR-IOV management (PF side) ─────────────────────────────────────── */
int  i40e_sriov_configure(struct i40e_priv *priv, int num_vfs);
int  i40e_sriov_init(void);

/* ── Mailbox (PF ↔ VF) ────────────────────────────────────────────────── */
int  i40e_mbx_send_to_vf(struct i40e_priv *priv, int vf_number,
                          const struct i40e_mbx_msg *msg);
int  i40e_mbx_recv_from_vf(struct i40e_priv *priv, int vf_number,
                            struct i40e_mbx_msg *msg);
int  i40e_vf_mbx_send_to_pf(struct i40e_vf_priv *priv,
                              const struct i40e_mbx_msg *msg);
int  i40e_vf_mbx_recv_from_pf(struct i40e_vf_priv *priv,
                                struct i40e_mbx_msg *msg);

#endif /* I40E_H */
