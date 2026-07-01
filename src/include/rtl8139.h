#ifndef RTL8139_H
#define RTL8139_H

#include "types.h"
#include "pci.h"
#include "netdevice.h"

/* ── PCI identifiers ──────────────────────────────────────────────── */
#define RTL8139_VENDOR  0x10EC
#define RTL8139_DEVICE  0x8139

/* ── PIO Register Map (BAR0 = I/O ports) ────────────────────────────
 *
 * The RTL8139 is a PCI Fast Ethernet controller that can be accessed
 * either via PIO (I/O ports on BAR0) or MMIO (memory-mapped on BAR1).
 * This driver uses PIO exclusively.
 *
 * All offsets are relative to the PIO base address from PCI BAR0.
 */
#define RTL_REG_IDR0       0x00  /* MAC Address bytes 0-5 (6 bytes) */
#define RTL_REG_IDR1       0x01
#define RTL_REG_IDR2       0x02
#define RTL_REG_IDR3       0x03
#define RTL_REG_IDR4       0x04
#define RTL_REG_IDR5       0x05
#define RTL_REG_MAR0       0x08  /* Multicast Address Filter (8 bytes) */
#define RTL_REG_MAR3       0x0B
#define RTL_REG_MAR4       0x0C
#define RTL_REG_MAR7       0x0F
#define RTL_REG_TDSTAT0    0x10  /* Transmit Status Descriptor 0 (4 bytes) */
#define RTL_REG_TDSTAT1    0x14  /* Transmit Status Descriptor 1 (4 bytes) */
#define RTL_REG_TDSTAT2    0x18  /* Transmit Status Descriptor 2 (4 bytes) */
#define RTL_REG_TDSTAT3    0x1C  /* Transmit Status Descriptor 3 (4 bytes) */
#define RTL_REG_TSAD0      0x20  /* Transmit Start Address 0 (4 bytes) */
#define RTL_REG_TSAD1      0x24  /* Transmit Start Address 1 (4 bytes) */
#define RTL_REG_TSAD2      0x28  /* Transmit Start Address 2 (4 bytes) */
#define RTL_REG_TSAD3      0x2C  /* Transmit Start Address 3 (4 bytes) */
#define RTL_REG_RBSTART    0x30  /* Receive Buffer Start Address (4 bytes) */
#define RTL_REG_CR         0x34  /* Command Register (1 byte) */
#define RTL_REG_CAPR       0x38  /* Current Address of Packet Read (2 bytes) */
#define RTL_REG_CBR        0x3A  /* Current Buffer Address (2 bytes, read-only) */
#define RTL_REG_IMR        0x3C  /* Interrupt Mask Register (2 bytes) */
#define RTL_REG_ISR        0x3E  /* Interrupt Status Register (2 bytes) */
#define RTL_REG_TCR        0x40  /* Transmit Configuration Register (4 bytes) */
#define RTL_REG_RCR        0x44  /* Receive Configuration Register (4 bytes) */
#define RTL_REG_TCTR       0x48  /* Timer Count Register (4 bytes) */
#define RTL_REG_MPC        0x4C  /* Missed Packet Counter (4 bytes) */
#define RTL_REG_9346CR     0x50  /* 93C46/93C56 Command Register (1 byte) */
#define RTL_REG_CONFIG1    0x52  /* Configuration Register 1 (1 byte) */
#define RTL_REG_CONFIG2    0x54  /* Configuration Register 2 (1 byte) */
#define RTL_REG_CONFIG3    0x56  /* Configuration Register 3 (1 byte) */
#define RTL_REG_CONFIG4    0x58  /* Configuration Register 4 (1 byte) */
#define RTL_REG_MULTIINTR  0x5C  /* Multiple Interrupt Select (2 bytes) */
#define RTL_REG_PCI_CTR    0xD0  /* PCI Control Register (2 bytes) */
#define RTL_REG_PCI_CTR2   0xD4  /* PCI Control Register 2 (2 bytes) */
#define RTL_REG_PCI_CTR3   0xDA  /* PCI Control Register 3 (2 bytes) */
#define RTL_REG_BMCR       0xE0  /* PHY Basic Mode Control (2 bytes) */
#define RTL_REG_BMSR       0xE2  /* PHY Basic Mode Status (2 bytes) */
#define RTL_REG_PHYID1     0xE4  /* PHY Identifier 1 (2 bytes) */
#define RTL_REG_PHYID2     0xE6  /* PHY Identifier 2 (2 bytes) */
#define RTL_REG_ANAR       0xE8  /* Auto-Negotiation Advertisement (2 bytes) */
#define RTL_REG_ANLPAR     0xEA  /* Auto-Negotiation Link Partner Ability (2 bytes) */
#define RTL_REG_ANER       0xEC  /* Auto-Negotiation Expansion (2 bytes) */
#define RTL_REG_DISCTIME   0xEE  /* Disconnect Time Counter (2 bytes) */

/* ── Command Register (CR) bits ──────────────────────────────────── */
#define RTL_CR_RST         (1U << 4)  /* Software Reset */
#define RTL_CR_RE          (1U << 3)  /* Receiver Enable */
#define RTL_CR_TE          (1U << 2)  /* Transmitter Enable */
#define RTL_CR_BUFE        (1U << 0)  /* Buffer Empty (read-only) */

/* ── Transmit Configuration Register (TCR) bits ───────────────────── */
#define RTL_TCR_MXDMA_16   0x00000700U  /* Max DMA burst: 16 dwords */
#define RTL_TCR_MXDMA_32   0x00000800U  /* Max DMA burst: 32 dwords */
#define RTL_TCR_MXDMA_64   0x00000900U  /* Max DMA burst: 64 dwords */
#define RTL_TCR_MXDMA_128  0x00000A00U  /* Max DMA burst: 128 dwords */
#define RTL_TCR_MXDMA_256  0x00000B00U  /* Max DMA burst: 256 dwords */
#define RTL_TCR_IFG2       0x80000000U  /* Interframe Gap bit 2 */
#define RTL_TCR_HWVERID    (7U << 23)   /* Hardware version ID mask */
#define RTL_TCR_HWVERID_SHIFT 23

/* ── Receive Configuration Register (RCR) bits ────────────────────── */
#define RTL_RCR_AAP         (1U << 0)  /* Accept All Packets (promiscuous) */
#define RTL_RCR_APM         (1U << 1)  /* Accept Physical Match */
#define RTL_RCR_AM          (1U << 2)  /* Accept Multicast */
#define RTL_RCR_AB          (1U << 3)  /* Accept Broadcast */
#define RTL_RCR_WRAP        (1U << 7)  /* Buffer Wrap */
#define RTL_RCR_MXDMA_16    0x00000700U
#define RTL_RCR_MXDMA_32    0x00000800U
#define RTL_RCR_MXDMA_64    0x00000900U
#define RTL_RCR_MXDMA_128   0x00000A00U
#define RTL_RCR_MXDMA_256   0x00000B00U
#define RTL_RCR_RBLEN_8K    (0U << 11)  /* RX buffer length: 8K */
#define RTL_RCR_RBLEN_16K   (1U << 11)  /* RX buffer length: 16K */
#define RTL_RCR_RBLEN_32K   (2U << 11)  /* RX buffer length: 32K */
#define RTL_RCR_RBLEN_64K   (3U << 11)  /* RX buffer length: 64K */

/* Default RCR value: accept physical match + broadcast + multicast,
 * 256-dword max DMA burst, 64K RX buffer */
#define RTL_RCR_DEFAULT (RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_AM | \
                         RTL_RCR_MXDMA_256 | RTL_RCR_RBLEN_64K)

/* ── Interrupt Status / Mask Register bits ────────────────────────── */
#define RTL_INTR_ROK        (1U << 0)  /* Receive OK */
#define RTL_INTR_RER        (1U << 1)  /* Receive Error */
#define RTL_INTR_TOK        (1U << 2)  /* Transmit OK */
#define RTL_INTR_TER        (1U << 3)  /* Transmit Error */
#define RTL_INTR_RDU        (1U << 4)  /* Receive Descriptor Unavailable (RX buffer overflow) */
#define RTL_INTR_TDU        (1U << 5)  /* Transmit Descriptor Unavailable */
#define RTL_INTR_LENCHG     (1U << 7)  /* Cable Length Change */
#define RTL_INTR_TIMEOUT    (1U << 14) /* Timer Interrupt */
#define RTL_INTR_SERR       (1U << 15) /* System Error */

/* ── TSD (Transmit Status Descriptor) bits ──────────────────────────── */
#define RTL_TSD_TOK         (1U << 15)  /* Transmit OK (read-only) */
#define RTL_TSD_TUN         (1U << 14)  /* Transmit FIFO Underrun */
#define RTL_TSD_OWN         (1U << 13)  /* Ownership: 1 = NIC owns, 0 = driver owns */
#define RTL_TSD_SIZE_MASK   0x00001FFF  /* Packet size in bytes */
#define RTL_TSD_CC_SHIFT    24          /* Collision Count */
#define RTL_TSD_CC_MASK     (0x0F << RTL_TSD_CC_SHIFT)

/* ── Transmit Start Address (TSAD) ownership bit ──────────────────── */
#define RTL_TSAD_OWN        (1U << 0)   /* When set, NIC owns the descriptor */

/* ── CONFIG1 bits ───────────────────────────────────────────────── */
#define RTL_CFG1_PMEN       (1U << 0)  /* Power Management Enable */
#define RTL_CFG1_LED0       (1U << 4)  /* LED select bit 0 */
#define RTL_CFG1_LED1       (1U << 5)  /* LED select bit 1 */
#define RTL_CFG1_MEMMAP     (1U << 6)  /* Memory Mapping Enable (read-only) */

/* ── CONFIG2 bits ───────────────────────────────────────────────── */
#define RTL_CFG2_PCI66      (1U << 0)  /* 66 MHz capable */
#define RTL_CFG2_BUSWIDTH   (1U << 5)  /* 0 = 32-bit, 1 = 64-bit */
#define RTL_CFG2_PCI64      (1U << 6)  /* PCI 64-bit capable */

/* ── 9346CR bits (EEPROM command) ────────────────────────────────── */
#define RTL_9346CR_EEM0     (1U << 0)  /* EEPROM mode bit 0 */
#define RTL_9346CR_EEM1     (1U << 1)  /* EEPROM mode bit 1 */
#define RTL_9346CR_EECS     (1U << 2)  /* EEPROM chip select */
#define RTL_9346CR_EESK     (1U << 3)  /* EEPROM clock */
#define RTL_9346CR_EEDI     (1U << 4)  /* EEPROM data in */
#define RTL_9346CR_EEDO     (1U << 5)  /* EEPROM data out */
#define RTL_9346CR_EECLK    0x03       /* Normal operation mode mask */
#define RTL_9346CR_EEM_NORM (RTL_9346CR_EEM0 | RTL_9346CR_EEM1) /* Normal op mode */

/* ── PHY BMCR bits (at PIO offset 0xE0) ──────────────────────────── */
#define RTL_BMCR_RESET      0x8000
#define RTL_BMCR_LOOPBACK   0x4000
#define RTL_BMCR_ANE        0x1000  /* Auto-Negotiation Enable */
#define RTL_BMCR_ANR        0x0200  /* Auto-Negotiation Restart */
#define RTL_BMCR_SPEED      (1U << 13) /* 0 = 10Mbps, 1 = 100Mbps */
#define RTL_BMCR_DUPLEX     (1U << 8)  /* 0 = half, 1 = full */

/* ── PHY BMSR bits (at PIO offset 0xE2) ──────────────────────────── */
#define RTL_BMSR_EXTENDED_CAP   0x0001  /* Extended register set capable */
#define RTL_BMSR_JABBER         0x0002  /* Jabber detect */
#define RTL_BMSR_LINK_STATUS    0x0004  /* Link up (latched — read twice for current) */
#define RTL_BMSR_AN_ABILITY     0x0008  /* Auto-negotiation supported */
#define RTL_BMSR_REMOTE_FAULT   0x0010  /* Remote fault */
#define RTL_BMSR_AN_COMPLETE    0x0020  /* Auto-negotiation complete */
#define RTL_BMSR_EXT_STATUS     0x0040  /* Extended status present */
#define RTL_BMSR_100T2_HD       0x0100  /* 100BASE-T2 half duplex capable */
#define RTL_BMSR_100T2_FD       0x0200  /* 100BASE-T2 full duplex capable */
#define RTL_BMSR_10T_HD         0x0400  /* 10BASE-T half duplex capable */
#define RTL_BMSR_10T_FD         0x0800  /* 10BASE-T full duplex capable */
#define RTL_BMSR_100TX_HD       0x1000  /* 100BASE-TX half duplex capable */
#define RTL_BMSR_100TX_FD       0x2000  /* 100BASE-TX full duplex capable */
#define RTL_BMSR_100T4          0x4000  /* 100BASE-T4 capable */
#define RTL_BMSR_PAUSE          0x8000  /* PAUSE operation supported */

/* Link state codes returned by rtl8139_check_link() */
#define RTL_LINK_DOWN   0
#define RTL_LINK_UP     1

/* Maximum number of TX descriptors (RTL8139 has 4) */
#define RTL8139_NUM_TX_DESC 4

/* TX buffer size (max Ethernet frame, 1500 + 14 header + 4 VLAN tag + padding) */
#define RTL8139_TX_BUF_SIZE 1536

/* RX buffer size (64K max) */
#define RTL8139_RX_BUF_SIZE (64 * 1024)

/* ── Driver statistics ──────────────────────────────────────────────── */
struct rtl8139_stats {
    uint64_t tx_packets;       /* total packets transmitted */
    uint64_t tx_bytes;         /* total bytes transmitted */
    uint64_t tx_errors;        /* transmit errors */
    uint64_t rx_packets;       /* total packets received */
    uint64_t rx_bytes;         /* total bytes received */
    uint64_t rx_errors;        /* receive errors */
    uint64_t tx_busy;          /* transmit ring full count */
};

/* ── Driver state ─────────────────────────────────────────────────── */
struct rtl8139_priv {
    uint16_t iobase;            /* PIO base address from PCI BAR0 */
    uint8_t  mac[6];            /* MAC address */
    int      irq_line;          /* IRQ line from PCI config */
    int      initialized;       /* 1 if hardware has been initialised */
    int      tx_cur;            /* next TX descriptor to use (0-3) */
    int      rx_cur;            /* current read position in RX ring buffer (byte offset) */
    struct rtl8139_stats stats; /* driver statistics */
    int      ifindex;           /* netdevice index (-1 if not registered) */

    /* TX buffers — one per descriptor, 16-byte aligned for DMA */
    uint8_t tx_bufs[RTL8139_NUM_TX_DESC][RTL8139_TX_BUF_SIZE]
        __attribute__((aligned(16)));

    /* RX ring buffer — 64KB, physically contiguous for DMA */
    uint8_t rx_buf[RTL8139_RX_BUF_SIZE]
        __attribute__((aligned(16)));

    /* Netdevice descriptor (registered if ifindex >= 0) */
    struct net_device ndev;
};

/* ── API ────────────────────────────────────────────────────────── */

/* Register access helpers (PIO via inb/inw/inl from io.h) */
uint8_t  rtl8139_readb(struct rtl8139_priv *priv, uint16_t reg);
uint16_t rtl8139_readw(struct rtl8139_priv *priv, uint16_t reg);
uint32_t rtl8139_readl(struct rtl8139_priv *priv, uint16_t reg);
void rtl8139_writeb(struct rtl8139_priv *priv, uint16_t reg, uint8_t val);
void rtl8139_writew(struct rtl8139_priv *priv, uint16_t reg, uint16_t val);
void rtl8139_writel(struct rtl8139_priv *priv, uint16_t reg, uint32_t val);

/* MAC address access */
void rtl8139_get_mac(struct rtl8139_priv *priv, uint8_t *mac);
int  rtl8139_set_mac(struct rtl8139_priv *priv, const uint8_t *mac);

/* Hardware control */
int  rtl8139_reset(struct rtl8139_priv *priv);
int  rtl8139_init_hw(struct rtl8139_priv *priv);
void rtl8139_shutdown(struct rtl8139_priv *priv);

/* PCI probe */
int  rtl8139_find_device(struct pci_device *pci_dev);
int  rtl8139_probe(struct rtl8139_priv *priv);

/* TX/RX ring buffer operations (netdevice callbacks) */
int  rtl8139_transmit(struct net_device *dev, const uint8_t *data, uint16_t len);
int  rtl8139_receive(struct net_device *dev, uint8_t *buf, uint16_t max_len);

/* Link state detection */
int  rtl8139_check_link(struct rtl8139_priv *priv);
const char *rtl8139_link_state_name(int link_state);

#endif /* RTL8139_H */
