#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "dma.h"
#include "net.h"
#include "netdevice.h"
#include "err.h"
#include "spinlock.h"
#include "module.h"
#ifdef MODULE
#endif

/* ---- RTL8139 C+ (C-plus) Mode Operation ----------------------------------
 *
 * The RTL8139 chipset exists in two major variants:
 *
 *   RTL8139C (aka "A/B/C revision") -- legacy mode only
 *   RTL8139C+ / RTL8139D          -- legacy + C+ mode
 *
 * -- Legacy mode (currently implemented) -----------------------------------
 *
 * - 4 fixed TX descriptor slots (TDSTAT0-3 / TSAD0-3 registers).
 * - Single contiguous receive ring buffer (RTL8139_RX_BUF_SIZE = 64 KB).
 * - CAPR/CBR registers track NIC/driver read positions in the RX ring.
 * - No hardware checksum offload or TSO; the driver must compute/verify
 *   IP/TCP/UDP checksums in software.
 * - Each TX descriptor is programmed by writing the packet length with
 *   the OWN bit to the TDSTAT register and the DMA address to TSAD.
 *
 * -- C+ mode (not yet implemented) -----------------------------------------
 *
 * The C+ mode uses a descriptor-ring architecture similar to the
 * RTL8169 Gigabit Ethernet controller.  When enabled via the C+ Command
 * Register (0xE0 on RTL8139C+; offset 0xE0 differs from the PHY BMCR
 * register that exists at the same offset on older chips), the NIC
 * ignores the legacy TDSTAT/TSAD/CAPR/CBR registers and instead uses
 * descriptor tables in host memory.
 *
 * C+ Key Registers (RTL8139D / 8139C+ only):
 *   TxConfig_8139C+  0x52  -- C+ TX configuration (IFG, DMA burst)
 *   RxConfig_8139C+  0xE0  -- C+ Command Register (enable C+ mode)
 *   TxDescStartAddr  0x64  -- Physical address of TX descriptor ring
 *   TxDescStartAddr_HI 0x68 -- High 32 bits (64-bit addressing)
 *   RxDescStartAddr  0xE4  -- Physical address of RX descriptor ring
 *   RxDescStartAddr_HI 0xE8 -- High 32 bits (64-bit addressing)
 *
 * C+ Descriptor Format (per descriptor, 16 bytes):
 *   struct rtl8139cp_desc {
 *       uint32_t addr_lo;    -- Buffer physical address, low 32 bits
 *       uint32_t addr_hi;    -- Buffer physical address, high 32 bits
 *       uint32_t len_opts;   -- Length (lower 16) + VLAN/TSO opts
 *       uint32_t status;     -- Ownership flag + status / error bits
 *   };
 *
 * C+ Features:
 * - TCP Segmentation Offload (TSO): The NIC segments large TCP packets
 *   in hardware up to 64 KB per descriptor.
 * - IP/TCP/UDP checksum offload: Hardware computes/verifies checksums
 *   (IPv4 checksum, TCP pseudo-header checksum, UDP checksum).
 * - VLAN hardware tagging: Insert/remove 802.1Q tags via descriptor
 *   options without copying data.
 * - Priority Queues: Two independent TX descriptor rings (high and
 *   normal priority) for QoS support.
 * - 64-bit DMA addressing: Separate high-32-bit descriptor address
 *   registers for buffers above 4 GB.
 * - RX checksum verification: NIC attaches a 16-bit hardware checksum
 *   field after each received frame, which the driver can use instead
 *   of computing software checksums.
 *
 * To enable C+ mode the driver must:
 *   1) Detect the chip revision via TCR[29:23] (HWVERID field).
 *   2) Allocate physically-contiguous descriptor rings in memory.
 *   3) Write ring base addresses to TxDescStartAddr / RxDescStartAddr.
 *   4) Set the C+ mode bit in the C+ Command Register (0xE0).
 *   5) Reconfigure TCR/RCR for descriptor-ring operation.
 *
 * The current driver implementation uses only legacy mode, which is
 * sufficient for basic network I/O on all RTL8139 chip revisions.
 * C+ mode support should be added when hardware checksum offload or
 * TSO performance is required.
 * ----------------------------------------------------------------------- */
/* ── Static driver state ─────────────────────────────────────────── */
static struct rtl8139_priv rtl8139_state;
static spinlock_t rtl8139_lock = SPINLOCK_INIT;

/* ── PIO register access helpers ─────────────────────────────────── */

uint8_t rtl8139_readb(struct rtl8139_priv *priv, uint16_t reg)
{
    return inb(priv->iobase + reg);
}

uint16_t rtl8139_readw(struct rtl8139_priv *priv, uint16_t reg)
{
    return inw(priv->iobase + reg);
}

uint32_t rtl8139_readl(struct rtl8139_priv *priv, uint16_t reg)
{
    return inl(priv->iobase + reg);
}

void rtl8139_writeb(struct rtl8139_priv *priv, uint16_t reg, uint8_t val)
{
    outb(priv->iobase + reg, val);
}

void rtl8139_writew(struct rtl8139_priv *priv, uint16_t reg, uint16_t val)
{
    outw(priv->iobase + reg, val);
}

void rtl8139_writel(struct rtl8139_priv *priv, uint16_t reg, uint32_t val)
{
    outl(priv->iobase + reg, val);
}

/* ── MAC address access ──────────────────────────────────────────── */

void rtl8139_get_mac(struct rtl8139_priv *priv, uint8_t *mac)
{
    mac[0] = rtl8139_readb(priv, RTL_REG_IDR0);
    mac[1] = rtl8139_readb(priv, RTL_REG_IDR1);
    mac[2] = rtl8139_readb(priv, RTL_REG_IDR2);
    mac[3] = rtl8139_readb(priv, RTL_REG_IDR3);
    mac[4] = rtl8139_readb(priv, RTL_REG_IDR4);
    mac[5] = rtl8139_readb(priv, RTL_REG_IDR5);
}

int rtl8139_set_mac(struct rtl8139_priv *priv, const uint8_t *mac)
{
    /* MAC address registers are writable only when the EEPROM is in
     * normal operating mode (9346CR bits EEM0|EEM1 both set). */
    uint8_t eeprom_mode = rtl8139_readb(priv, RTL_REG_9346CR);
    if ((eeprom_mode & 0x03) != RTL_9346CR_EECLK) {
        /* Not in normal mode — write to 9346CR to enter normal op mode
         * (EEM0|EEM1 = 0x03, which is the "normal operation" state). */
        rtl8139_writeb(priv, RTL_REG_9346CR, 0x03);
    }

    rtl8139_writeb(priv, RTL_REG_IDR0, mac[0]);
    rtl8139_writeb(priv, RTL_REG_IDR1, mac[1]);
    rtl8139_writeb(priv, RTL_REG_IDR2, mac[2]);
    rtl8139_writeb(priv, RTL_REG_IDR3, mac[3]);
    rtl8139_writeb(priv, RTL_REG_IDR4, mac[4]);
    rtl8139_writeb(priv, RTL_REG_IDR5, mac[5]);

    /* Restore EEPROM mode */
    rtl8139_writeb(priv, RTL_REG_9346CR, eeprom_mode);
    return 0;
}

/* ── Multicast filter configuration ────────────────────────────── */

void rtl8139_set_multicast_filter(struct rtl8139_priv *priv)
{
    /* The RTL8139 uses a 64-bit hash-based multicast address filter
     * (MAR0-MAR7).  When RCR_AM is set, the NIC computes a 6-bit hash
     * index (CRC bits 30:26 of the destination address) and tests the
     * corresponding bit in the 64-bit MAR.
     *
     * After power-on or system-wide reset, the MAR is undefined or all
     * zeros, which would cause the NIC to reject ALL multicast packets
     * even though the driver advertises multicast support.  Writing all
     * ones (accept-all-multicast) ensures full multicast reception — the
     * simplest safe default for a driver that does not yet implement
     * fine-grained multicast address registration. */
    rtl8139_writeb(priv, RTL_REG_MAR0, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR1, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR2, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR3, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR4, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR5, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR6, 0xFF);
    rtl8139_writeb(priv, RTL_REG_MAR7, 0xFF);
}

/* ── Hardware reset ──────────────────────────────────────────────── */

int rtl8139_reset(struct rtl8139_priv *priv)
{
    int timeout;

    /* Issue software reset via CR register */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_RST);

    /* Wait for reset to complete (RST bit self-clears) */
    timeout = 1000000;
    while ((rtl8139_readb(priv, RTL_REG_CR) & RTL_CR_RST) && timeout > 0) {
        io_wait();
        timeout--;
    }

    if (timeout == 0) {
        kprintf("  rtl8139: reset timed out\n");
        return -ETIMEDOUT;
    }

    kprintf("  rtl8139: reset complete\n");
    return 0;
}

/* ── Hardware initialization ─────────────────────────────────────── */

int rtl8139_init_hw(struct rtl8139_priv *priv)
{
    uint32_t tcr, rcr;
    int ret;
    int i;

    /* Step 1: Reset the chip */
    ret = rtl8139_reset(priv);
    if (ret < 0)
        return ret;

    /* Step 2: Read MAC address from on-chip registers */
    rtl8139_get_mac(priv, priv->mac);
    kprintf("  rtl8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5]);

    /* Step 3: Unlock configuration registers (9346CR to normal mode) */
    rtl8139_writeb(priv, RTL_REG_9346CR, 0x03);

    /* Step 4: Set CONFIG1 — turn off optional features */
    rtl8139_writeb(priv, RTL_REG_CONFIG1,
                   (uint8_t)(rtl8139_readb(priv, RTL_REG_CONFIG1) & ~RTL_CFG1_PMEN));

    /* Step 5: Initialize TX buffers and reset TX descriptor pointers */
    for (i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        memset(priv->tx_bufs[i], 0, RTL8139_TX_BUF_SIZE);
    }
    priv->tx_cur = 0;

    /* Step 6: Initialize RX ring buffer */
    memset(priv->rx_buf, 0, RTL8139_RX_BUF_SIZE);
    priv->rx_cur = 0;

    /* Step 6b: Validate DMA addressing.  RBSTART and TSAD0-3 are
     * 32-bit registers — the RTL8139 cannot DMA to buffers above the
     * 4 GB boundary.  Reject at init time rather than silently
     * truncating the (uint64_t) physical address in the casts below,
     * which would make the NIC DMA to the wrong location. */
    for (i = 0; i < RTL8139_NUM_TX_DESC; i++) {
        if (VIRT_TO_PHYS(priv->tx_bufs[i]) > (uint64_t)DMA_BIT_MASK(32)) {
            kprintf("  rtl8139: TX buffer %d phys 0x%llx exceeds 32-bit "
                    "DMA mask\n", i,
                    (unsigned long long)VIRT_TO_PHYS(priv->tx_bufs[i]));
            return -EIO;
        }
    }
    if (VIRT_TO_PHYS(priv->rx_buf) > (uint64_t)DMA_BIT_MASK(32)) {
        kprintf("  rtl8139: RX buffer phys 0x%llx exceeds 32-bit DMA mask\n",
                (unsigned long long)VIRT_TO_PHYS(priv->rx_buf));
        return -EIO;
    }

    /* Write physical address of RX buffer to RBSTART (must be done
     * before enabling the receiver).  Proven to fit the 32-bit DMA
     * mask by the check above. */
    rtl8139_writel(priv, RTL_REG_RBSTART,
                   (uint32_t)VIRT_TO_PHYS(priv->rx_buf));

    /* Set CAPR to 0 — no packets consumed yet */
    rtl8139_writew(priv, RTL_REG_CAPR, 0);

    /* Step 7: Configure Transmit Configuration Register (TCR) */
    tcr = rtl8139_readl(priv, RTL_REG_TCR);
    tcr &= ~(0x0F << 8);          /* Clear MXDMA field */
    tcr |= RTL_TCR_MXDMA_256;     /* 256-dword max DMA burst */
    tcr &= ~RTL_TCR_IFG2;         /* Clear IFG2 */
    rtl8139_writel(priv, RTL_REG_TCR, tcr);

    /* Step 8: Configure Receive Configuration Register (RCR).
     * Enable the WRAP bit so the NIC wraps around when the ring buffer
     * is full instead of stopping. */
    rcr = RTL_RCR_DEFAULT | RTL_RCR_WRAP;
    rtl8139_writel(priv, RTL_REG_RCR, rcr);

    /* Step 9: Clear missed packet counter */
    rtl8139_readl(priv, RTL_REG_MPC);

    /* Step 9b: Initialize multicast address filter to accept-all.
     * Must be done while config registers are unlocked (step 3). */
    rtl8139_set_multicast_filter(priv);

    /* Step 10: Lock configuration registers */
    rtl8139_writeb(priv, RTL_REG_9346CR, RTL_9346CR_EECLK);

    /* Step 11: Enable RX and TX */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_TE | RTL_CR_RE);

    kprintf("  rtl8139: hardware initialized (TX rings: %d slots, RX ring: %d bytes)\n",
            RTL8139_NUM_TX_DESC, RTL8139_RX_BUF_SIZE);
    priv->initialized = 1;

    /* Step 12: Check initial link state */
    {
        int ls = rtl8139_check_link(priv);
        kprintf("  rtl8139: link %s\n", rtl8139_link_state_name(ls));
        if (ls == RTL_LINK_UP) {
            uint16_t bmsr = rtl8139_readw(priv, RTL_REG_BMSR);
            unsigned an_complete = (bmsr & RTL_BMSR_AN_COMPLETE) ? 1 : 0;
            kprintf("  rtl8139: auto-negotiation %s\n",
                    an_complete ? "complete" : "in progress");
        }
    }

    return 0;
}

/* ── Shutdown ────────────────────────────────────────────────────── */

void rtl8139_shutdown(struct rtl8139_priv *priv)
{
    /* Unregister from netdevice layer */
    if (priv->ifindex >= 0) {
        netif_unregister(priv->ifindex);
        priv->ifindex = -1;
    }

    /* Disable RX and TX */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_RST);

    /* Mask all interrupts */
    rtl8139_writew(priv, RTL_REG_IMR, 0);

    /* Acknowledge any pending interrupt */
    rtl8139_readw(priv, RTL_REG_ISR);

    priv->initialized = 0;
    kprintf("  rtl8139: shutdown complete\n");
}

/* ── PCI probe ───────────────────────────────────────────────────── */

int rtl8139_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(RTL8139_VENDOR, RTL8139_DEVICE, pci_dev);
    if (ret < 0)
        return -ENODEV;

    return 0;
}

int rtl8139_probe(struct rtl8139_priv *priv)
{
    struct pci_device pci_dev;
    uint16_t iobase;
    int ret;

    /* Find the RTL8139 on PCI bus */
    ret = rtl8139_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  rtl8139: device not found\n");
        return ret;
    }

    /* BAR0 contains the I/O port base address.
     * Mask off the lower bits (1 = I/O space indicator). */
    iobase = (uint16_t)(pci_dev.bar[0] & 0xFFFC);
    if (iobase == 0) {
        kprintf("  rtl8139: invalid PIO base from BAR0\n");
        return -ENODEV;
    }

    priv->iobase  = iobase;
    priv->irq_line = pci_dev.irq;
    priv->initialized = 0;
    priv->tx_cur = 0;

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&pci_dev);

    kprintf("  rtl8139: found at I/O base 0x%04x, IRQ %d\n",
            priv->iobase, priv->irq_line);

    return 0;
}

/* ── Netdevice transmit callback ──────────────────────────────────── */

int rtl8139_transmit(struct net_device *dev,
                     const uint8_t *data, uint16_t len)
{
    struct rtl8139_priv *priv;
    int slot;
    uint32_t tsd;
    int timeout;
    uint64_t flags;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct rtl8139_priv *)dev->priv;
    if (!priv->initialized)
        return -EIO;

    if (len > RTL8139_TX_BUF_SIZE)
        return -EMSGSIZE;

    slot = priv->tx_cur;

    /* Quick check: if the next descriptor is still owned by the NIC
     * (ring full), return BUSY so the upper layer can retry. */
    tsd = rtl8139_readl(priv, (uint16_t)(RTL_REG_TDSTAT0 + slot * 4));
    if (tsd & RTL_TSD_OWN) {
        /* Check if NIC has finished since we last looked */
        timeout = 1000;
        do {
            __asm__ volatile("pause");
            tsd = rtl8139_readl(priv, (uint16_t)(RTL_REG_TDSTAT0 + slot * 4));
            if (!(tsd & RTL_TSD_OWN))
                break;
        } while (--timeout > 0);

        if (timeout <= 0) {
            priv->stats.tx_busy++;
            return NETDEV_TX_BUSY;  /* ring is full, try again later */
        }
    }

    spinlock_irqsave_acquire(&rtl8139_lock, &flags);

    /* Copy packet data to the TX buffer */
    memcpy(priv->tx_bufs[slot], data, len);
    if (len < RTL8139_TX_BUF_SIZE)
        memset(priv->tx_bufs[slot] + len, 0,
               RTL8139_TX_BUF_SIZE - len);

    /* Give NIC ownership of the descriptor by writing the packet
     * length with the OWN bit set to the Transmit Status register. */
    rtl8139_writel(priv, (uint16_t)(RTL_REG_TDSTAT0 + slot * 4),
                   (uint32_t)len | RTL_TSD_OWN);

    /* Write the physical buffer address to the TX Start Address
     * register — this kicks off the transmission.  The buffer is
     * statically allocated and was validated to fit the 32-bit DMA
     * mask in rtl8139_init_hw(), so the truncation is lossless. */
    rtl8139_writel(priv, (uint16_t)(RTL_REG_TSAD0 + slot * 4),
                   (uint32_t)VIRT_TO_PHYS(priv->tx_bufs[slot]));

    /* Advance to next descriptor slot (0-3 round-robin) */
    priv->tx_cur = (slot + 1) % RTL8139_NUM_TX_DESC;

    priv->stats.tx_packets++;
    priv->stats.tx_bytes += len;

    spinlock_irqsave_release(&rtl8139_lock, flags);
    return 0;
}

/* ── Netdevice receive callback ───────────────────────────────────── */

int rtl8139_receive(struct net_device *dev,
                    uint8_t *buf, uint16_t max_len)
{
    struct rtl8139_priv *priv;
    uint16_t cbr;
    int received = 0;
    uint64_t flags;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct rtl8139_priv *)dev->priv;
    if (!priv->initialized)
        return -EIO;

    spinlock_irqsave_acquire(&rtl8139_lock, &flags);

    /* Read Current Buffer Address — the byte offset (from RBSTART)
     * where the NIC has written up to.  This is a 16-bit value that
     * wraps at RTL8139_RX_BUF_SIZE. */
    cbr = rtl8139_readw(priv, RTL_REG_CBR);

    /* Process packets until we catch up with the NIC */
    while (priv->rx_cur != cbr) {
        uint32_t pkt_hdr;
        uint16_t pkt_status, pkt_len;
        int pkt_size;
        uint16_t data_len;

        /* Guard: if there isn't room for a 4-byte header at rx_cur,
         * wrap around to the beginning of the ring buffer. */
        if (priv->rx_cur + 4 > RTL8139_RX_BUF_SIZE) {
            priv->rx_cur = 0;
            if (priv->rx_cur == cbr)
                break;
        }

        /* Read the 4-byte packet header (little-endian words):
         *   [15:0]  = frame status (bit 0 = ROK = Receive OK)
         *   [31:16] = frame length (includes the 4-byte header,
         *              does NOT include FCS/CRC) */
        pkt_hdr = *(volatile uint32_t *)&priv->rx_buf[priv->rx_cur];
        pkt_status = (uint16_t)(pkt_hdr & 0xFFFF);
        pkt_len    = (uint16_t)(pkt_hdr >> 16);

        /* Sanity check: length must be at least 4 (header itself).
         * The pkt_len is uint16_t, so the maximum is 65535 which
         * fits within the 64K ring buffer. */
        if (pkt_len < 4) {
            /* Corruption — pkt_len can never be < 4 for a valid frame */
            priv->rx_cur = (int)cbr;
            priv->stats.rx_errors++;
            break;
        }

        /* Packet footprint in the ring buffer (dword-aligned).
         * Compute as uint32_t to avoid uint16_t overflow. */
        {
            uint32_t pkt_size_32 = ((uint32_t)pkt_len + 3) & ~3U;
            /* Validate: dword-aligned size must not exceed the ring buffer.
             * If pkt_size >= RTL8139_RX_BUF_SIZE, advancing rx_cur by
             * pkt_size wraps back to the original position, creating an
             * infinite loop.  This catches corrupt descriptors where
             * pkt_len >= 65533 (the dword-aligned size wraps to 65536). */
            if (pkt_size_32 >= RTL8139_RX_BUF_SIZE) {
                priv->rx_cur = (int)cbr;
                priv->stats.rx_errors++;
                break;
            }
            pkt_size = (int)pkt_size_32;
        }

        if (pkt_status & RTL_INTR_ROK) {
            /* Valid packet — copy data (skip 4-byte header) */
            data_len = pkt_len - 4;
            if (data_len > max_len - (uint16_t)received)
                data_len = (uint16_t)(max_len - received);

            /* Handle ring buffer wrap for the payload copy */
            if (priv->rx_cur + 4 + data_len > RTL8139_RX_BUF_SIZE) {
                uint16_t first_chunk;
                first_chunk = (uint16_t)(RTL8139_RX_BUF_SIZE
                                         - priv->rx_cur - 4);
                memcpy(buf + received,
                       &priv->rx_buf[priv->rx_cur + 4], first_chunk);
                memcpy(buf + received + first_chunk,
                       priv->rx_buf, data_len - first_chunk);
            } else {
                memcpy(buf + received,
                       &priv->rx_buf[priv->rx_cur + 4], data_len);
            }
            received += data_len;
            priv->stats.rx_packets++;
            priv->stats.rx_bytes += data_len;
        } else {
            priv->stats.rx_errors++;
        }

        /* Advance the read pointer */
        priv->rx_cur += pkt_size;
        if (priv->rx_cur >= RTL8139_RX_BUF_SIZE)
            priv->rx_cur -= RTL8139_RX_BUF_SIZE;

        /* Re-read CBR in case the NIC has written more data
         * while we were processing (non-blocking poll) */
        cbr = rtl8139_readw(priv, RTL_REG_CBR);
    }

    /* Update CAPR to tell the NIC we've consumed up to this point.
     * The Linux convention is CAPR = rx_cur - 16, which tells the
     * NIC that it can write new packets up to that offset.
     * The uint16_t cast handles wraparound when rx_cur < 16. */
    {
        uint16_t capr = (uint16_t)(priv->rx_cur - 16);
        rtl8139_writew(priv, RTL_REG_CAPR, capr);
    }

    spinlock_irqsave_release(&rtl8139_lock, flags);

    if (received > 0)
        return received;

    return 0;
}

/* ── Link state detection ──────────────────────────────────────────── */

int rtl8139_check_link(struct rtl8139_priv *priv)
{
    uint16_t bmsr;
    int link_state;

    /* BMSR bit 2 (Link Status) is latching per IEEE 802.3 — it stays
     * cleared after a link fault until the register is read.  Read
     * once to clear the latch, then again for the current status. */
    bmsr = rtl8139_readw(priv, RTL_REG_BMSR);
    (void)bmsr;

    bmsr = rtl8139_readw(priv, RTL_REG_BMSR);
    link_state = (bmsr & RTL_BMSR_LINK_STATUS) ? RTL_LINK_UP : RTL_LINK_DOWN;

    /* Reflect the current link state in the netdevice flags so the
     * network stack knows whether this interface is operational. */
    if (link_state == RTL_LINK_UP)
        priv->ndev.flags |= IFF_RUNNING;
    else
        priv->ndev.flags &= ~(unsigned)IFF_RUNNING;

    return link_state;
}

const char *rtl8139_link_state_name(int link_state)
{
    if (link_state == RTL_LINK_UP)
        return "up";
    else
        return "down";
}

/* ── Module / device init entry point ────────────────────────────── */

int rtl8139_init(void)
{
    int ret;

    memset(&rtl8139_state, 0, sizeof(rtl8139_state));
    rtl8139_state.ifindex = -1;

    ret = rtl8139_probe(&rtl8139_state);
    if (ret < 0)
        return ret;

    ret = rtl8139_init_hw(&rtl8139_state);
    if (ret < 0) {
        kprintf("  rtl8139: hardware init failed\n");
        return ret;
    }

    /* Register with the netdevice layer so the network stack can
     * send and receive frames through this interface. */
    {
        struct net_device *ndev = &rtl8139_state.ndev;

        memset(ndev, 0, sizeof(*ndev));
        snprintf(ndev->name, sizeof(ndev->name), "eth1");
        memcpy(ndev->mac, rtl8139_state.mac, 6);
        ndev->transmit = rtl8139_transmit;
        ndev->receive  = rtl8139_receive;
        ndev->mtu      = 1500;
        ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
        ndev->priv     = &rtl8139_state;

        ret = netif_register(ndev);
        if (ret >= 0) {
            rtl8139_state.ifindex = ret;
            kprintf("  rtl8139: registered as eth1 (ifindex=%d)\n", ret);
        } else {
            kprintf("  rtl8139: netif_register failed\n");
        }
    }

    kprintf("rtl8139: driver loaded\n");
    return 0;
}

void rtl8139_exit(void)
{
    if (rtl8139_state.initialized)
        rtl8139_shutdown(&rtl8139_state);
    kprintf("rtl8139: driver unloaded\n");
}

#ifdef MODULE
#endif

module_init(rtl8139_init);
module_exit(rtl8139_exit);

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Realtek RTL8139 Fast Ethernet driver (TX/RX ring buffer, link state detection)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v000010ECd00008139*");
#endif
