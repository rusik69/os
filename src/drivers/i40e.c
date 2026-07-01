/*
 * src/drivers/i40e.c — Intel XL710/X710 40GbE Controller (i40e) PF driver
 *
 * Physical Function (PF) driver for the Intel XL710 40GbE and X710 10GbE
 * Ethernet controllers.  Implements PCI device identification, MMIO-based
 * device control, single-queue TX/RX descriptor rings, MSI-X interrupt
 * handling via pci_setup_interrupts(), and netdevice registration.
 *
 * Task 12: i40e: PF driver with MSI-X
 *   - PCI device identification (vendor 0x8086, devices 0x1572/0x1581)
 *   - MMIO mapping of BAR0 (4 MB region)
 *   - Device reset and initialisation
 *   - MAC address reading from PFMAC registers
 *   - Single TX/RX descriptor ring setup
 *   - MSI-X interrupt setup via pci_setup_interrupts() (best-effort
 *     fallback: MSI-X → MSI → INTx)
 *   - PFINT_ICR0-based interrupt handler
 *   - Netdevice registration as "eth4"
 *   - Interrupt rearm via PFINT_DYN_CTL
 */

#include "i40e.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "net.h"
#include "netdevice.h"
#include "pmm.h"
#include "errno.h"
#include "err.h"
#include "kernel.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Static driver state ──────────────────────────────────────────── */
static struct i40e_priv i40e_state;

/* ── Forward declarations ─────────────────────────────────────────── */
static int i40e_netdev_transmit(struct net_device *dev,
                                const uint8_t *data, uint16_t len);
static int i40e_netdev_receive(struct net_device *dev,
                               uint8_t *buf, uint16_t max_len);

/* ── Device reset ──────────────────────────────────────────────────── */

/* i40e_reset_hw - Perform a global port reset.
 *
 * Initiates a Port-level Reset (PFR) via GLGEN_RSTCTL, then waits
 * for the reset to complete by polling GLGEN_STATUS until the
 * PFR_STAT bit clears.  Returns 0 on success, -ETIMEDOUT on timeout.
 */
int i40e_reset_hw(struct i40e_priv *priv)
{
    uint32_t reg;
    int timeout;

    /* Request a Port-level Reset */
    reg = i40e_readl(priv, I40E_GLGEN_RSTCTL);
    reg |= I40E_GLGEN_RSTCTL_PFR;
    i40e_writel(priv, I40E_GLGEN_RSTCTL, reg);

    /* Wait for reset to complete (PFR_STAT bit self-clears) */
    timeout = 500000;
    while (timeout > 0) {
        reg = i40e_readl(priv, I40E_GLGEN_STATUS);
        if (!(reg & I40E_GLGEN_STATUS_PFR_STAT))
            break;
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        kprintf("  i40e: reset timeout (GLGEN_STATUS=0x%08x)\n", reg);
        return -ETIMEDOUT;
    }

    /* Post-reset delay */
    {
        int d;
        for (d = 0; d < 10000; d++)
            io_wait();
    }

    return 0;
}

/* ── MAC address ──────────────────────────────────────────────────── */

/* i40e_get_mac - Read the MAC address from the PFMAC registers.
 *
 * The MAC address is stored in PRT_PFMAC_L (low 32 bits) and
 * PRT_PFMAC_H (high 16 bits). */
static void i40e_get_mac(struct i40e_priv *priv)
{
    uint32_t mac_l, mac_h;

    mac_l = i40e_readl(priv, I40E_PRT_PFMAC_L);
    mac_h = i40e_readl(priv, I40E_PRT_PFMAC_H);

    priv->mac[0] = (uint8_t)(mac_l & 0xFF);
    priv->mac[1] = (uint8_t)((mac_l >> 8) & 0xFF);
    priv->mac[2] = (uint8_t)((mac_l >> 16) & 0xFF);
    priv->mac[3] = (uint8_t)((mac_l >> 24) & 0xFF);
    priv->mac[4] = (uint8_t)(mac_h & 0xFF);
    priv->mac[5] = (uint8_t)((mac_h >> 8) & 0xFF);

    kprintf("  i40e: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5]);
}

/* ── TX ring setup ───────────────────────────────────────────────── */

/* i40e_setup_tx_ring - Allocate and initialise a TX descriptor ring.
 *
 * Allocates physically contiguous memory for the 32-byte descriptor
 * ring, registers it with hardware via QTX_BASE_ADDR_H/L registers,
 * and initialises the descriptor ring with status bits set to DD
 * (Descriptor Done) indicating the slots are ready for use.
 * Returns 0 on success, negative errno on failure. */
int i40e_setup_tx_ring(struct i40e_priv *priv, int q_idx)
{
    struct i40e_txq *txq;
    uint64_t phys;
    void *virt;
    int i;

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return -EINVAL;

    txq = &priv->txq[q_idx];
    txq->size = I40E_TX_RING_SIZE;
    txq->next = 0;
    txq->clean = 0;

    /* Allocate physically contiguous memory for the descriptor ring.
     * Each descriptor is 32 bytes, so ring = 256 × 32 = 8192 bytes = 2 pages */
    phys = (uint64_t)pmm_alloc_frames(2);
    if (phys == 0) {
        kprintf("  i40e: failed to allocate TX ring %d\n", q_idx);
        return -ENOMEM;
    }

    virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)phys);
    if (virt == NULL) {
        pmm_free_frames_contiguous(phys, 2);
        return -ENOMEM;
    }

    txq->descs = (struct i40e_tx_desc *)virt;
    txq->descs_phys = phys;

    /* Initialise all descriptors: set DD so hardware owns them initially */
    memset(txq->descs, 0, I40E_TX_RING_SIZE * sizeof(struct i40e_tx_desc));
    for (i = 0; i < I40E_TX_RING_SIZE; i++)
        txq->descs[i].olinfo_status = I40E_TXD_ST_DD;

    /* Program TX queue registers */
    i40e_writel(priv, I40E_QTX_BASE_ADDR_L(q_idx),
                (uint32_t)(phys & 0xFFFFFFFF));
    i40e_writel(priv, I40E_QTX_BASE_ADDR_H(q_idx),
                (uint32_t)(phys >> 32));
    i40e_writel(priv, I40E_QTX_LENGTH(q_idx),
                I40E_TX_RING_SIZE * sizeof(struct i40e_tx_desc));
    i40e_writel(priv, I40E_QTX_HEAD(q_idx), 0);
    i40e_writel(priv, I40E_QTX_TAIL(q_idx), 0);
    i40e_writel(priv, I40E_QTX_ENABLED(q_idx), 1);

    kprintf("  i40e: TX ring %d at phys=0x%llx virt=%p size=%d\n",
            q_idx, (unsigned long long)phys, virt, I40E_TX_RING_SIZE);

    return 0;
}

/* i40e_teardown_tx_ring - Release TX ring resources. */
void i40e_teardown_tx_ring(struct i40e_priv *priv, int q_idx)
{
    struct i40e_txq *txq;

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return;

    txq = &priv->txq[q_idx];

    /* Disable queue */
    i40e_writel(priv, I40E_QTX_ENABLED(q_idx), 0);

    /* Free descriptor ring */
    if (txq->descs_phys != 0) {
        pmm_free_frames_contiguous(txq->descs_phys, 2);
        txq->descs_phys = 0;
    }
    txq->descs = NULL;
    txq->size = 0;
}

/* ── RX ring setup ───────────────────────────────────────────────── */

/* i40e_setup_rx_ring - Allocate and initialise an RX descriptor ring.
 *
 * Allocates physically contiguous ring memory and a pool of receive
 * buffers.  Each buffer is I40E_RX_BUF_SIZE bytes.  The ring is
 * programmed via QRX_BASE_ADDR_H/L.  Returns 0 on success, negative
 * errno on failure. */
int i40e_setup_rx_ring(struct i40e_priv *priv, int q_idx)
{
    struct i40e_rxq *rxq;
    uint64_t phys;
    void *virt;
    int i, page_frames;
    uint64_t pool_phys;
    void *pool_virt;

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return -EINVAL;

    rxq = &priv->rxq[q_idx];
    rxq->size = I40E_RX_RING_SIZE;
    rxq->next = 0;
    rxq->cur = 0;

    /* Allocate descriptor ring (2 pages for 256 × 32 bytes) */
    phys = (uint64_t)pmm_alloc_frames(2);
    if (phys == 0) {
        kprintf("  i40e: failed to allocate RX ring %d\n", q_idx);
        return -ENOMEM;
    }

    virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)phys);
    if (virt == NULL) {
        pmm_free_frames_contiguous(phys, 2);
        return -ENOMEM;
    }

    rxq->descs = (struct i40e_rx_desc *)virt;
    rxq->descs_phys = phys;
    memset(rxq->descs, 0, I40E_RX_RING_SIZE * sizeof(struct i40e_rx_desc));

    /* Allocate buffer pool for RX buffers */
    page_frames = I40E_RX_BUF_PAGE_COUNT;
    pool_phys = (uint64_t)pmm_alloc_frames(page_frames);
    if (pool_phys == 0) {
        kprintf("  i40e: failed to allocate RX buf pool %d\n", q_idx);
        pmm_free_frames_contiguous(phys, 2);
        return -ENOMEM;
    }

    pool_virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)pool_phys);
    if (pool_virt == NULL) {
        pmm_free_frames_contiguous(phys, 2);
        pmm_free_frames_contiguous(pool_phys, (size_t)page_frames);
        return -ENOMEM;
    }

    /* Fill RX ring with buffers and set descriptor addresses */
    for (i = 0; i < I40E_RX_RING_SIZE; i++) {
        rxq->buffers[i] = (uint8_t *)pool_virt + i * I40E_RX_BUF_SIZE;
        rxq->buf_phys[i] = pool_phys + (uint64_t)(i * I40E_RX_BUF_SIZE);
        rxq->descs[i].addr = rxq->buf_phys[i];
        rxq->descs[i].status_error = 0;
    }

    /* Program RX queue registers */
    i40e_writel(priv, I40E_QRX_BASE_ADDR_L(q_idx),
                (uint32_t)(phys & 0xFFFFFFFF));
    i40e_writel(priv, I40E_QRX_BASE_ADDR_H(q_idx),
                (uint32_t)(phys >> 32));
    i40e_writel(priv, I40E_QRX_LENGTH(q_idx),
                I40E_RX_RING_SIZE * sizeof(struct i40e_rx_desc));
    i40e_writel(priv, I40E_QRX_HEAD(q_idx), 0);
    i40e_writel(priv, I40E_QRX_TAIL(q_idx), I40E_RX_RING_SIZE - 1);
    i40e_writel(priv, I40E_QRX_ENABLED(q_idx), 1);

    kprintf("  i40e: RX ring %d at phys=0x%llx virt=%p size=%d "
            "buf_pool=%d pages\n",
            q_idx, (unsigned long long)phys, virt,
            I40E_RX_RING_SIZE, page_frames);

    return 0;
}

/* i40e_teardown_rx_ring - Release RX ring resources. */
void i40e_teardown_rx_ring(struct i40e_priv *priv, int q_idx)
{
    struct i40e_rxq *rxq;

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return;

    rxq = &priv->rxq[q_idx];

    /* Disable queue */
    i40e_writel(priv, I40E_QRX_ENABLED(q_idx), 0);

    /* Free descriptor ring */
    if (rxq->descs_phys != 0) {
        pmm_free_frames_contiguous(rxq->descs_phys, 2);
        rxq->descs_phys = 0;
    }
    rxq->descs = NULL;
    rxq->size = 0;
}

/* ── Hardware initialisation ──────────────────────────────────────── */

/* i40e_init_hw - Initialise hardware.
 *
 * Sequence: reset, allocate buffer pool, set up TX/RX rings, enable
 * receiver and transmitter.  Returns 0 on success, negative errno
 * on failure. */
int i40e_init_hw(struct i40e_priv *priv)
{
    int ret;

    /* Reset the device */
    ret = i40e_reset_hw(priv);
    if (ret < 0)
        return ret;

    /* Read MAC address */
    i40e_get_mac(priv);

    /* Set up single TX/RX queue (queue 0) */
    ret = i40e_setup_tx_ring(priv, 0);
    if (ret < 0)
        return ret;

    ret = i40e_setup_rx_ring(priv, 0);
    if (ret < 0) {
        i40e_teardown_tx_ring(priv, 0);
        return ret;
    }

    /* Enable the receiver (SRCRXE) */
    i40e_writel(priv, I40E_PRT_SRCRXE, I40E_PRT_SRCRXE_ENA);

    priv->present = 1;

    kprintf("  i40e: hardware initialized (1 TX, 1 RX queue)\n");
    return 0;
}

/* i40e_shutdown_hw - Shut down hardware.
 *
 * Disables TX/RX queues and receiver. */
void i40e_shutdown_hw(struct i40e_priv *priv)
{
    int q;

    if (!priv->present)
        return;

    /* Disable receiver */
    i40e_writel(priv, I40E_PRT_SRCRXE, 0);

    /* Teardown queues */
    for (q = 0; q < priv->num_tx_queues; q++)
        i40e_teardown_tx_ring(priv, q);
    for (q = 0; q < priv->num_rx_queues; q++)
        i40e_teardown_rx_ring(priv, q);

    priv->present = 0;
    kprintf("  i40e: hardware shutdown\n");
}

/* ── Data path ────────────────────────────────────────────────────── */

/* i40e_transmit_ring - Transmit a single packet on a TX ring.
 *
 * Places the packet data into the next available TX descriptor and
 * advances the tail pointer to trigger transmission.
 * Returns 0 on success, -ENOMEM if the ring is full. */
int i40e_transmit_ring(struct i40e_priv *priv, int q_idx,
                       const uint8_t *data, uint16_t len)
{
    struct i40e_txq *txq;
    struct i40e_tx_desc *desc;
    int idx, next;

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return -EINVAL;

    txq = &priv->txq[q_idx];
    idx = txq->next;

    /* Check if this descriptor is available (DD still set means done) */
    desc = &txq->descs[idx];
    if (!(desc->olinfo_status & I40E_TXD_ST_DD)) {
        priv->tx_errors++;
        return -ENOMEM;  /* ring full */
    }

    /* Copy packet data into the descriptor's buffer area.
     * For simplicity we use a small embedded buffer approach:
     * the descriptor addr points to the TX buffer slot. */
    {
        uint64_t buf_phys;
        void *buf_virt;

        /* Allocate a temporary buffer for this frame */
        buf_phys = (uint64_t)pmm_alloc_frames(1);
        if (buf_phys == 0)
            return -ENOMEM;

        buf_virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)buf_phys);
        if (buf_virt == NULL) {
            pmm_free_frames_contiguous(buf_phys, 1);
            return -ENOMEM;
        }

        memcpy(buf_virt, data, len);

        desc->addr = buf_phys;
        desc->cmd_type_len = (uint32_t)((I40E_TXD_CMD_EOP |
                                          I40E_TXD_CMD_IFCS |
                                          I40E_TXD_CMD_RS |
                                          I40E_TXD_CMD_DEXT) << 24) |
                             (uint32_t)(len & 0x0003FFFF);
        desc->olinfo_status = 0;  /* clear DD — hardware owns the desc */

        /* Track for cleanup */
        txq->clean = idx;  /* simplified: clean this idx on next completion */
    }

    /* Advance producer */
    next = (idx + 1) & (I40E_TX_RING_SIZE - 1);
    txq->next = next;

    /* Ring doorbell — write tail register */
    i40e_writel(priv, I40E_QTX_TAIL(q_idx), next);

    priv->tx_packets++;
    priv->tx_bytes += len;

    return 0;
}

/* i40e_receive_ring - Receive a single packet from an RX ring.
 *
 * Checks the next RX descriptor for data availability (DD bit).
 * If data is ready, copies it into @buf (up to @max_len bytes)
 * and recycles the descriptor buffer.  Returns the number of bytes
 * received, 0 if no data available, or negative errno on error. */
int i40e_receive_ring(struct i40e_priv *priv, int q_idx,
                      uint8_t *buf, uint16_t max_len)
{
    struct i40e_rxq *rxq;
    struct i40e_rx_desc *desc;
    int idx, len = 0;

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return -EINVAL;

    rxq = &priv->rxq[q_idx];
    idx = rxq->next;

    desc = &rxq->descs[idx];
    if (!(desc->status_error & I40E_RXD_ST_DD))
        return 0;  /* no packet ready */

    /* Check if this is a complete packet (EOP) */
    if (!(desc->status_error & I40E_RXD_ST_EOP)) {
        /* For now, skip partial packets (would need reassembly) */
        desc->status_error = 0;
        goto skip;
    }

    /* Get the received length */
    len = (int)(desc->length & 0x3FFF);
    if (len > (int)max_len)
        len = (int)max_len;

    if (desc->status_error & (I40E_RXD_ERR_CE | I40E_RXD_ERR_SE |
                               I40E_RXD_ERR_RXE)) {
        priv->rx_errors++;
        goto skip;
    }

    /* Copy received data */
    if (len > 0 && buf != NULL)
        memcpy(buf, rxq->buffers[idx], (size_t)len);

    priv->rx_packets++;
    priv->rx_bytes += (uint64_t)len;

skip:
    /* Recycle the descriptor — clear status, re-arm with buffer addr */
    desc->status_error = 0;
    desc->addr = rxq->buf_phys[idx];

    /* Advance consumer index */
    rxq->next = (idx + 1) & (I40E_RX_RING_SIZE - 1);

    /* Update tail to give descriptors back to hardware */
    i40e_writel(priv, I40E_QRX_TAIL(q_idx),
                (uint32_t)rxq->next);

    return len;
}

/* ── Netdevice callbacks ──────────────────────────────────────────── */

static int i40e_netdev_transmit(struct net_device *dev,
                                const uint8_t *data, uint16_t len)
{
    struct i40e_priv *priv;

    if (dev == NULL || dev->priv == NULL)
        return -EINVAL;

    priv = (struct i40e_priv *)dev->priv;
    if (!priv->present)
        return -ENODEV;

    return i40e_transmit_ring(priv, 0, data, len);
}

static int i40e_netdev_receive(struct net_device *dev,
                               uint8_t *buf, uint16_t max_len)
{
    struct i40e_priv *priv;

    if (dev == NULL || dev->priv == NULL)
        return -EINVAL;

    priv = (struct i40e_priv *)dev->priv;
    if (!priv->present)
        return -ENODEV;

    return i40e_receive_ring(priv, 0, buf, max_len);
}

/* ── Interrupt handling ───────────────────────────────────────────── */

/* i40e_irq_handler - Main interrupt handler for the i40e PF.
 *
 * Handles MSI-X or legacy interrupts.  Reads PFINT_ICR0 to
 * determine the cause, acknowledges by clearing relevant bits,
 * then triggers RX polling if a receive event is indicated. */
void i40e_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    struct i40e_priv *priv;
    uint32_t icr0;

    priv = &i40e_state;
    if (!priv->present)
        return;

    /* Read interrupt cause register to see what happened */
    icr0 = i40e_readl(priv, I40E_PFINT_ICR0);

    if (icr0 & I40E_ICR0_LSC) {
        kprintf("  i40e: link status change\n");
    }

    if (icr0 & I40E_ICR0_RX_QUEUE) {
        /* Receive queue event — the stack will poll via netif_recv */
        /* No further action needed here since we use polling receive */
    }

    if (icr0 & I40E_ICR0_TX_QUEUE) {
        /* TX completion — clean completed descriptors */
        struct i40e_txq *txq = &priv->txq[0];
        struct i40e_tx_desc *desc;
        int clean_idx = txq->clean;

        desc = &txq->descs[clean_idx];
        if (desc->olinfo_status & I40E_TXD_ST_DD) {
            /* Descriptor completed — free its buffer if allocated */
            if (desc->addr != 0) {
                pmm_free_frames_contiguous(desc->addr, 1);
                desc->addr = 0;
            }
            desc->olinfo_status = I40E_TXD_ST_DD;  /* re-arm */
        }
    }

    if (icr0 & I40E_ICR0_RESET_DONE) {
        kprintf("  i40e: global reset completed\n");
    }

    /* Re-arm interrupts by writing to PFINT_DYN_CTL */
    i40e_writel(priv, I40E_PFINT_DYN_CTLN(0),
                I40E_PFINT_DYN_CTLN_INTENA |
                (I40E_PFINT_DYN_CTLN_ITR_NONE << 3) |
                I40E_PFINT_DYN_CTLN_INTENA_M);
}

/* ── Interrupt setup ──────────────────────────────────────────────── */

/* i40e_setup_interrupts - Configure interrupts for the i40e device.
 *
 * Uses the PCI subsystem's best-effort interrupt setup (tries MSI-X
 * first, then MSI, then legacy INTx).  Returns 0 on success, negative
 * errno on total failure. */
int i40e_setup_interrupts(struct i40e_priv *priv, struct pci_device *pci_dev)
{
    int ret;

    memset(&priv->int_cfg, 0, sizeof(priv->int_cfg));
    ret = pci_setup_interrupts(pci_dev, &priv->int_cfg, i40e_irq_handler);
    if (ret < 0) {
        kprintf("  i40e: interrupt setup failed (%d)\n", ret);
        return ret;
    }

    /* Store MSI-X vector info for later use */
    if (priv->int_cfg.type == 2) {
        /* MSI-X mode — we have multiple vectors available */
        priv->msix_nvecs = priv->int_cfg.n_vectors;
        priv->msix_vector_base = priv->int_cfg.vector;
        kprintf("  i40e: MSI-X with %d vector(s), base=%d\n",
                priv->msix_nvecs, priv->msix_vector_base);
    } else {
        /* MSI (type=1) or INTx (type=0) — single vector */
        priv->msix_nvecs = 0;
        priv->msix_vector_base = -1;
        kprintf("  i40e: legacy interrupt (type=%d, vector=%d)\n",
                priv->int_cfg.type, priv->int_cfg.vector);
    }

    return 0;
}

/* i40e_teardown_interrupts - Tear down interrupts for the i40e device. */
void i40e_teardown_interrupts(struct i40e_priv *priv)
{
    if (priv->int_cfg.type == 2) {
        /* Disable MSI-X in PCI config space */
        struct pci_device pci_dev;

        memset(&pci_dev, 0, sizeof(pci_dev));
        if (i40e_find_device(&pci_dev) == 0)
            pci_disable_msix(&pci_dev);

        priv->msix_nvecs = 0;
        priv->msix_vector_base = -1;
    }

    memset(&priv->int_cfg, 0, sizeof(priv->int_cfg));
}

/* ── Statistics ───────────────────────────────────────────────────── */

void i40e_print_stats(struct i40e_priv *priv)
{
    if (!priv->present)
        return;

    kprintf("  i40e: TX=%llu pkts / %llu bytes, "
            "RX=%llu pkts / %llu bytes, "
            "ERR(TX=%llu RX=%llu)\n",
            (unsigned long long)priv->tx_packets,
            (unsigned long long)priv->tx_bytes,
            (unsigned long long)priv->rx_packets,
            (unsigned long long)priv->rx_bytes,
            (unsigned long long)priv->tx_errors,
            (unsigned long long)priv->rx_errors);
}

/* ── PCI probe ────────────────────────────────────────────────────── */

/* i40e_find_device - Find an i40e-compatible device on the PCI bus.
 *
 * Tries XL710, X710, and alternate XL710 device IDs.
 * Returns 0 on success, -ENODEV if no compatible device found. */
int i40e_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(I40E_VENDOR, I40E_DEV_XL710, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(I40E_VENDOR, I40E_DEV_X710, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(I40E_VENDOR, I40E_DEV_XL710_A, pci_dev);
    if (ret >= 0)
        return 0;

    return -ENODEV;
}

/* i40e_probe - Probe and initialise an i40e device.
 *
 * Finds the device on the PCI bus, maps MMIO BAR0, enables bus
 * mastering, sets up interrupts, and initialises the hardware.
 * Returns 0 on success, negative errno on failure. */
int i40e_probe(struct i40e_priv *priv)
{
    struct pci_device pci_dev;
    uint64_t mmio_phys;
    int ret;

    /* Find an i40e-compatible device on the PCI bus */
    ret = i40e_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  i40e: device not found\n");
        return ret;
    }

    /* BAR0 contains the MMIO base address (4 MB region for PF) */
    mmio_phys = (uint64_t)(pci_dev.bar[0] & ~0xFULL);
    if (mmio_phys == 0) {
        kprintf("  i40e: invalid MMIO base from BAR0\n");
        return -ENODEV;
    }

    priv->mmio_phys  = mmio_phys;
    priv->mmio_base  = (uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)mmio_phys);
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;
    priv->device_id  = pci_dev.device_id;

    /* Configure queue counts (single queue for now) */
    priv->num_tx_queues = 1;
    priv->num_rx_queues = 1;

    kprintf("  i40e: found device 0x%04x at %02x:%02x.%d "
            "BAR0=0x%llx IRQ=%d\n",
            pci_dev.device_id,
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            (unsigned long long)mmio_phys, pci_dev.irq);

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&pci_dev);

    /* Set up interrupts (MSI-X preferred, fallback MSI/INTx) */
    ret = i40e_setup_interrupts(priv, &pci_dev);
    if (ret < 0)
        return ret;

    /* Initialise hardware (reset, rings, etc.) */
    ret = i40e_init_hw(priv);
    if (ret < 0) {
        i40e_teardown_interrupts(priv);
        return ret;
    }

    return 0;
}

/* ── Module / device init entry point ─────────────────────────────── */

int i40e_init(void)
{
    int ret;

    memset(&i40e_state, 0, sizeof(i40e_state));
    i40e_state.ifindex = -1;

    ret = i40e_probe(&i40e_state);
    if (ret < 0)
        return ret;

    /* ── Register with netdevice layer ───────────────────────────── */
    {
        struct net_device *ndev = &i40e_state.ndev;

        /* Try eth4 (after e1000=eth0, vmxnet3=eth1, rtl8139=eth2,
         * igb=eth3) */
        if (netif_name_to_index("eth4") < 0) {
            memset(ndev, 0, sizeof(*ndev));
            snprintf(ndev->name, sizeof(ndev->name), "eth4");
            memcpy(ndev->mac, i40e_state.mac, 6);
            ndev->transmit = i40e_netdev_transmit;
            ndev->receive  = i40e_netdev_receive;
            ndev->mtu      = 1500;
            ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev->priv     = &i40e_state;

            int ifindex = netif_register(ndev);
            if (ifindex >= 0) {
                i40e_state.ifindex = ifindex;
                kprintf("  i40e: registered as netdevice ifindex=%d\n",
                        ifindex);
            } else {
                kprintf("  i40e: netdevice registration failed\n");
                i40e_shutdown_hw(&i40e_state);
                i40e_teardown_interrupts(&i40e_state);
                return -EIO;
            }
        } else {
            /* eth4 already taken — try next available */
            char name[NETDEV_NAME_MAX];
            int i;

            for (i = 4; i < NETDEV_MAX; i++) {
                snprintf(name, sizeof(name), "eth%d", i);
                if (netif_name_to_index(name) < 0) {
                    memset(ndev, 0, sizeof(*ndev));
                    snprintf(ndev->name, sizeof(ndev->name), "%s", name);
                    memcpy(ndev->mac, i40e_state.mac, 6);
                    ndev->transmit = i40e_netdev_transmit;
                    ndev->receive  = i40e_netdev_receive;
                    ndev->mtu      = 1500;
                    ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
                    ndev->priv     = &i40e_state;

                    int ifindex = netif_register(ndev);
                    if (ifindex >= 0) {
                        i40e_state.ifindex = ifindex;
                        kprintf("  i40e: registered as netdevice "
                                "ifindex=%d (%s)\n", ifindex, name);
                        break;
                    }
                }
            }

            if (i40e_state.ifindex < 0) {
                kprintf("  i40e: no available netdevice slot\n");
                i40e_shutdown_hw(&i40e_state);
                i40e_teardown_interrupts(&i40e_state);
                return -ENOSPC;
            }
        }
    }

    kprintf("i40e: driver loaded (XL710/X710 PF, 1 TX/RX queue, "
            "MSI-X=%d)\n",
            i40e_state.msix_nvecs);
    return 0;
}

void i40e_exit(void)
{
    if (i40e_state.present) {
        /* Unregister from netdevice layer */
        if (i40e_state.ifindex >= 0) {
            netif_unregister(i40e_state.ifindex);
            i40e_state.ifindex = -1;
        }

        /* Shut down hardware */
        i40e_shutdown_hw(&i40e_state);

        /* Tear down interrupts */
        i40e_teardown_interrupts(&i40e_state);

        i40e_state.present = 0;
    }
    kprintf("i40e: driver unloaded\n");
}

#ifdef MODULE
module_init(i40e_init);
module_exit(i40e_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Intel XL710/X710 40GbE PF driver with MSI-X");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v00008086d00001572*");
MODULE_ALIAS("pci:v00008086d00001581*");
MODULE_ALIAS("pci:v00008086d00001580*");
#endif
