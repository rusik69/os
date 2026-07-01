#ifndef MLX4_H
#define MLX4_H

#include "types.h"
#include "pci.h"
#include "netdevice.h"

/*
 * src/include/mlx4.h — Mellanox ConnectX-3 (mlx4) NIC driver header
 *
 * Supports the Mellanox ConnectX-3 (0x1003), ConnectX-3 Pro (0x1007),
 * and related VF devices.  Provides firmware command interface, MMIO
 * register access, and basic device initialization.
 *
 * Task 14: mlx4: firmware command interface
 *   - PCI device identification
 *   - MMIO mapping of BAR0 (1 MB region)
 *   - Firmware command interface via CMD/GO registers
 *   - Mailbox-based command I/O (QUERY_FW, QUERY_DEV_CAP, QUERY_HCA)
 *   - Initialisation sequence: QUERY_FW → QUERY_DEV_CAP → QUERY_HCA → INIT_HCA
 */

/* ── PCI identifiers ─────────────────────────────────────────────────── */
#define MLX4_VENDOR         0x15B3  /* Mellanox Technologies */
#define MLX4_DEV_CONNECTX3  0x1003  /* MT27500 ConnectX-3 */
#define MLX4_DEV_CONNECTX3P 0x1007  /* MT27510/20 ConnectX-3 Pro */
#define MLX4_DEV_VF         0x1004  /* ConnectX-3 Virtual Function */
#define MLX4_DEV_IOV        0x1013  /* ConnectX-3 IOV */

/* ── MMIO BAR0 size (1 MB) ───────────────────────────────────────────── */
#define MLX4_BAR0_SIZE        0x100000

/* ── Firmware command interface register offsets (BAR0) ───────────────── */
#define MLX4_CMD_HCR          0x10    /* HCR command register base (8 bytes) */
#define MLX4_CMD_PARAM        0x18    /* Parameter low register (4 bytes) */
#define MLX4_CMD_PARAM_HI     0x1C    /* Parameter high register (4 bytes) */
#define MLX4_CMD_DB           0x20    /* Doorbell register (4 bytes) */
#define MLX4_CMD_STAT         0x24    /* Status register (4 bytes) */

/* HCR register (at 0x10) layout (64-bit):
 *   [63:48] - command opcode (16 bits)
 *   [47:44] - input modifier (4 bits)
 *   [43:16] - reserved / inline data
 *   [15:0]  - output modifier / length
 *
 * For simplicity we define 32-bit halves:
 *   CMD_CTL (offset 0x10) — command + go bit
 *   CMD_IN  (offset 0x14) — input modifier / inline data
 */
#define MLX4_HCR           0x10
#define MLX4_HCR_IN        0x14
#define MLX4_HCR_PARAM     0x18
#define MLX4_HCR_PARAM_HI  0x1C
#define MLX4_HCR_DB        0x20
#define MLX4_HCR_STAT      0x24

/* CMD_CTL register bits */
#define MLX4_HCR_GO_BIT          (1U << 31)   /* Go bit — initiate command */
#define MLX4_HCR_OPCODE_SHIFT    16            /* Opcode shift in CMD_CTL */
#define MLX4_HCR_OPCODE_MASK     0xFFFFUL      /* Opcode mask */
#define MLX4_HCR_OPCODE(op)      (((uint32_t)(op) & MLX4_HCR_OPCODE_MASK) << MLX4_HCR_OPCODE_SHIFT)

/* Status register values */
#define MLX4_STAT_SUCCESS        0
#define MLX4_STAT_RESERVED       1
#define MLX4_STAT_BAD_OP         2
#define MLX4_STAT_BAD_PARAM      3
#define MLX4_STAT_BAD_SYS_STATE  4
#define MLX4_STAT_INTERNAL_ERR   5
#define MLX4_STAT_TIMEOUT        6
#define MLX4_STAT_BAD_RESOURCE   7

/* ── Firmware command opcodes ──────────────────────────────────────────── */
#define MLX4_CMD_QUERY_FW        0x0001  /* Query firmware info */
#define MLX4_CMD_QUERY_DEV_CAP   0x0003  /* Query device capabilities */
#define MLX4_CMD_QUERY_HCA       0x0006  /* Query HCA (Host Channel Adapter) */
#define MLX4_CMD_INIT_HCA        0x0007  /* Initialise HCA */
#define MLX4_CMD_CLOSE_HCA       0x0008  /* Close HCA */
#define MLX4_CMD_QUERY_PORT      0x0010  /* Query port properties */
#define MLX4_CMD_SET_PORT        0x0011  /* Configure port */
#define MLX4_CMD_ACCESS_REG      0x0012  /* Register access */
#define MLX4_CMD_MAD_IFC         0x0013  /* Management Datagram IFC */

/* ── Mailbox interface (large commands) ───────────────────────────────── */
#define MLX4_MAILBOX_SIZE        4096
#define MLX4_MAILBOX_ADDR_IN     (0x20000ULL)   /* BAR0 offset for input mailbox */
#define MLX4_MAILBOX_ADDR_OUT    (0x21000ULL)   /* BAR0 offset for output mailbox */
#define MLX4_NUM_MAILBOXES       2

/* ── Capability bit flags (from QUERY_DEV_CAP) ─────────────────────────── */
#define MLX4_DEV_CAP_FLAG_64B_EQE       (1ULL << 31)
#define MLX4_DEV_CAP_FLAG_64B_CQE       (1ULL << 30)
#define MLX4_DEV_CAP_FLAG_PORT_TYPE_ETH (1ULL << 16)
#define MLX4_DEV_CAP_FLAG_PORT_TYPE_IB  (1ULL << 17)
#define MLX4_DEV_CAP_FLAG_RSS           (1ULL << 8)
#define MLX4_DEV_CAP_FLAG_VLAN          (1ULL << 9)
#define MLX4_DEV_CAP_FLAG_TSO           (1ULL << 10)
#define MLX4_DEV_CAP_FLAG_LRO           (1ULL << 11)

/* ── Port types ───────────────────────────────────────────────────────── */
#define MLX4_PORT_TYPE_NONE     0
#define MLX4_PORT_TYPE_IB       1
#define MLX4_PORT_TYPE_ETH      2
#define MLX4_PORT_TYPE_AUTO     3

/* ── Maximum supported configuration ──────────────────────────────────── */
#define MLX4_MAX_TX_QUEUES      8
#define MLX4_MAX_RX_QUEUES      8
#define MLX4_MAX_PORTS          2

/* ── Descriptor ring sizes ────────────────────────────────────────────── */
#define MLX4_TX_RING_SIZE       256
#define MLX4_RX_RING_SIZE       256
#define MLX4_RX_BUF_SIZE        2048

/* ── Command mailbox structure ─────────────────────────────────────────── *
 * The command descriptor is placed at the mailbox BAR address.
 * Layout (first 16 bytes):
 *   [0-7]   - input / output parameters (64-bit)
 *   [8-15]  - command-specific data
 * For QUERY_FW, the response occupies the first 32 bytes of the mailbox.
 */
struct mlx4_mailbox {
    volatile uint32_t data[MLX4_MAILBOX_SIZE / 4];
} __attribute__((packed));

/* ── Firmware info (from QUERY_FW) ────────────────────────────────────── */
struct mlx4_fw_info {
    uint64_t fw_revision;
    uint64_t fw_pages;
    uint64_t total_ram;
    uint32_t max_cmds;
    uint8_t  fw_version[32];
};

/* ── Device capabilities (from QUERY_DEV_CAP) ─────────────────────────── */
struct mlx4_dev_cap {
    uint64_t flags;
    uint32_t max_qps;
    uint32_t max_mpts;
    uint32_t max_mtts;
    uint32_t max_cqs;
    uint32_t max_eqs;
    uint32_t max_mcg;
    uint32_t max_srqs;
    uint32_t max_counters;
    uint32_t num_ports;
    uint8_t  port_type[MLX4_MAX_PORTS];
};

/* ── HCA parameters (from QUERY_HCA) ──────────────────────────────────── */
struct mlx4_hca_params {
    uint32_t qp_table_size;
    uint32_t eq_table_size;
    uint32_t cq_table_size;
    uint32_t mpt_table_size;
    uint32_t mtt_table_size;
    uint32_t mcg_table_size;
    uint32_t srq_table_size;
    uint32_t num_qps;
    uint32_t num_cqs;
    uint32_t num_eqs;
    uint32_t num_mpts;
    uint32_t num_mtts;
    uint32_t num_srqs;
    uint32_t num_mcgs;
    uint32_t num_uars;
    uint64_t uar_page_size;
};

/* ── Port info (from QUERY_PORT) ──────────────────────────────────────── */
struct mlx4_port_info {
    uint8_t  mac[6];
    uint32_t link_speed;      /* 0 = unknown, 1 = 1G, 10 = 10G, 40 = 40G, 56 = 56G */
    uint32_t link_state;      /* 0 = down, 1 = up */
    uint8_t  port_type;       /* MLX4_PORT_TYPE_* */
};

/* ── TX descriptor (16 bytes, BlueFlame-optimised) ────────────────────── *
 * The mlx4 uses shared memory via send queue rings.
 * Each WQE (Work Queue Element) is 16 bytes.
 */
struct mlx4_tx_desc {
    uint64_t addr;          /* Physical address of packet data */
    uint32_t length;        /* Data length (in bytes) */
    uint32_t flags;         /* Control flags (EOP, VL15, etc.) */
} __attribute__((packed));

/* TX descriptor flags */
#define MLX4_TXD_FLAG_EOP         (1U << 0)   /* End of Packet */
#define MLX4_TXD_FLAG_ICRC        (1U << 1)   /* Insert CRC */
#define MLX4_TXD_FLAG_IP_CSUM     (1U << 2)   /* IP checksum offload */
#define MLX4_TXD_FLAG_TCP_CSUM    (1U << 3)   /* TCP checksum offload */
#define MLX4_TXD_FLAG_UDP_CSUM    (1U << 4)   /* UDP checksum offload */
#define MLX4_TXD_FLAG_VLAN        (1U << 5)   /* VLAN tag insertion */
#define MLX4_TXD_FLAG_TSO         (1U << 6)   /* TSO offload */

/* ── RX WQE (Work Queue Element, 16 bytes) ────────────────────────────── */
struct mlx4_rx_desc {
    uint64_t addr;          /* Physical address of receive buffer */
    uint32_t length;        /* Buffer length */
    uint32_t flags;         /* Status flags */
} __attribute__((packed));

/* ── TX queue state ───────────────────────────────────────────────────── */
struct mlx4_txq {
    struct mlx4_tx_desc *descs;        /* Virtual addr of TX descriptor ring */
    uint64_t             descs_phys;   /* Physical addr of TX descriptor ring */
    int                  size;         /* Number of descriptors */
    int                  next;         /* Producer index */
    int                  clean;        /* Consumer index */
};

/* ── RX queue state ───────────────────────────────────────────────────── */
struct mlx4_rxq {
    struct mlx4_rx_desc *descs;        /* Virtual addr of RX descriptor ring */
    uint64_t             descs_phys;   /* Physical addr of RX descriptor ring */
    int                  size;         /* Number of descriptors */
    int                  next;         /* Consumer index */
    uint8_t             *buffers[MLX4_RX_RING_SIZE];  /* Virt addr of RX buffers */
    uint64_t             buf_phys[MLX4_RX_RING_SIZE]; /* Phys addr of RX buffers */
};

/* ── Driver private data ──────────────────────────────────────────────── */
struct mlx4_priv {
    int             present;           /* Device found and initialised */
    uintptr_t       mmio_base;         /* Virtual address of BAR0 MMIO */
    uint64_t        mmio_phys;         /* Physical address of BAR0 */
    int             irq_line;          /* Legacy IRQ line */
    int             ifindex;           /* Registered netdevice index */

    /* Identification */
    uint16_t        device_id;
    uint16_t        subsys_vendor;
    uint16_t        subsys_device;

    /* MAC address */
    uint8_t         mac[6];

    /* Firmware info */
    struct mlx4_fw_info     fw;
    struct mlx4_dev_cap     cap;
    struct mlx4_hca_params  hca;
    int                     fw_initted;  /* INIT_HCA called successfully */

    /* Port info */
    int                     num_ports;
    struct mlx4_port_info   port[MLX4_MAX_PORTS];

    /* Queue configuration */
    int             num_tx_queues;
    int             num_rx_queues;
    struct mlx4_txq txq[MLX4_MAX_TX_QUEUES];
    struct mlx4_rxq rxq[MLX4_MAX_RX_QUEUES];

    /* Statistics */
    uint64_t        tx_packets;
    uint64_t        rx_packets;
    uint64_t        tx_bytes;
    uint64_t        rx_bytes;
    uint64_t        tx_errors;
    uint64_t        rx_errors;

    /* Netdevice */
    struct net_device ndev;

    /* Mailbox pool */
    struct mlx4_mailbox *mbox_in;
    struct mlx4_mailbox *mbox_out;
    uint64_t             mbox_in_phys;
    uint64_t             mbox_out_phys;
};

/* ══════════════════════════════════════════════════════════════════════
 *  Register access helpers
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_readl - Read a 32-bit MMIO register from BAR0 */
static inline uint32_t mlx4_readl(const struct mlx4_priv *priv,
                                  uint32_t reg)
{
    return *((volatile uint32_t *)(priv->mmio_base + reg));
}

/* mlx4_writel - Write a 32-bit MMIO register to BAR0 */
static inline void mlx4_writel(struct mlx4_priv *priv,
                               uint32_t reg, uint32_t val)
{
    *((volatile uint32_t *)(priv->mmio_base + reg)) = val;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Function prototypes
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Device discovery ───────────────────────────────────────────────── */
int mlx4_find_device(struct pci_device *pci_dev);

/* ── Lifecycle ──────────────────────────────────────────────────────── */
int mlx4_probe(struct mlx4_priv *priv);
int mlx4_init(void);
void mlx4_exit(void);

/* ── Firmware command interface ─────────────────────────────────────── */
int mlx4_cmd(struct mlx4_priv *priv, uint16_t opcode,
             uint64_t param_in, uint64_t *param_out,
             int use_mailbox, uint64_t timeout_ms);
int mlx4_cmd_get_success(uint32_t status);

/* ── Firmware commands (high-level helpers) ─────────────────────────── */
int mlx4_query_fw(struct mlx4_priv *priv);
int mlx4_query_dev_cap(struct mlx4_priv *priv);
int mlx4_query_hca(struct mlx4_priv *priv);
int mlx4_init_hca(struct mlx4_priv *priv);
int mlx4_close_hca(struct mlx4_priv *priv);
int mlx4_query_port(struct mlx4_priv *priv, int port);

/* ── Hardware initialisation ────────────────────────────────────────── */
int mlx4_init_hw(struct mlx4_priv *priv);
void mlx4_shutdown_hw(struct mlx4_priv *priv);

/* ── Netdevice glue ─────────────────────────────────────────────────── */
/* mlx4_netdev_transmit and mlx4_netdev_receive are internal callbacks
 * set in the net_device struct, declared static in mlx4.c */

#endif /* MLX4_H */
