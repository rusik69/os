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

/* ── External SR-IOV functions (from sriov.c, no header yet) ──────── */
extern int sriov_probe_pf(int bus, int dev, int func);
extern int sriov_enable_vfs(int bus, int dev, int func, int num_vfs);
extern int sriov_disable_vfs(int bus, int dev, int func);

#include "module.h"
#ifdef MODULE
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
static int i40e_initialized = 0;
static int i40e_vf_initialized = 0;

/* i40e_vf_shared_priv - Pointer to PF context for VF to access HW.
 * In the unikernel model, the VF driver shares the PF's MMIO mapping
 * to manage its assigned queues.  The VF uses queue indices beyond
 * the PF's own queues. */
static struct i40e_priv *i40e_vf_shared_pf = NULL;

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

    i40e_initialized = 1;

    /* If SR-IOV is available, initialise VF support and try to
     * bring up a VF netdevice alongside the PF. */
    i40e_sriov_init();
    i40e_vf_shared_pf = &i40e_state;

    /* Try to initialise the VF driver (may fail if VF support not
     * enabled or no VF BAR allocated — that's OK). */
    if (i40e_vf_init() == 0)
        i40e_vf_initialized = 1;

    return 0;
}

void i40e_exit(void)
{
    /* Shut down VF driver first if initialised */
    if (i40e_vf_initialized) {
        i40e_vf_exit();
        i40e_vf_initialized = 0;
    }
    i40e_vf_shared_pf = NULL;

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
    i40e_initialized = 0;
    kprintf("i40e: driver unloaded\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  SR-IOV VF Driver
 *
 *  Virtual Function (VF) driver for the Intel XL710/X710.  In the
 *  unikernel model the PF and VF run in the same address space, so the
 *  VF driver shares the PF's MMIO mapping but manages its own queue
 *  pairs, netdevice, and MAC address.  Communication between the PF
 *  and VF is performed via the mailbox register interface.
 *
 *  Task 13: i40e: SR-IOV VF support
 *    - VF PCI device identification (0x154C / 0x1570)
 *    - VF hardware initialisation and reset
 *    - VF TX/RX descriptor rings (using queue indices beyond PF's)
 *    - VF netdevice registration (eth5)
 *    - VF↔PF mailbox protocol for control messages
 *    - PF-side SR-IOV enablement via sriov_configure
 * ══════════════════════════════════════════════════════════════════════ */

/* ── VF driver state ────────────────────────────────────────────────── */
static struct i40e_vf_priv i40e_vf_state;
static int i40e_vf_devices_found = 0;

/* ── VF MMIO accessors (reuse PF readl/writel via shared pointer) ────── */
static inline uint32_t i40e_vf_readl(struct i40e_vf_priv *priv, uint32_t reg)
{
    (void)priv;
    if (i40e_vf_shared_pf == NULL)
        return 0;
    return i40e_readl(i40e_vf_shared_pf, reg);
}

static inline void i40e_vf_writel(struct i40e_vf_priv *priv, uint32_t reg,
                                   uint32_t val)
{
    (void)priv;
    if (i40e_vf_shared_pf == NULL)
        return;
    i40e_writel(i40e_vf_shared_pf, reg, val);
}

/* ── Mailbox: VF → PF (send from VF to PF) ──────────────────────────── */

int i40e_vf_mbx_send_to_pf(struct i40e_vf_priv *priv,
                             const struct i40e_mbx_msg *msg)
{
    int vf = priv->vf_number;
    uint32_t ctl;

    if (i40e_vf_shared_pf == NULL || vf < 0)
        return -ENODEV;

    /* Check if previous message was ACK'd */
    ctl = i40e_readl(i40e_vf_shared_pf, I40E_VF_MBX_CTL(vf));
    if (ctl & I40E_MBX_CTL_VF2PF)
        return -EAGAIN;  /* previous message not consumed */

    /* Write mailbox data registers */
    i40e_writel(i40e_vf_shared_pf, I40E_VF_MBX_DATA0(vf), msg->type);
    i40e_writel(i40e_vf_shared_pf, I40E_VF_MBX_DATA1(vf), msg->data0);
    i40e_writel(i40e_vf_shared_pf, I40E_VF_MBX_DATA2(vf), msg->data1);

    /* Signal PF by setting VF2PF flag */
    i40e_writel(i40e_vf_shared_pf, I40E_VF_MBX_CTL(vf),
                I40E_MBX_CTL_VF2PF);
    return 0;
}

/* ── Mailbox: VF → PF (receive on PF side) ──────────────────────────── */

int i40e_mbx_recv_from_vf(struct i40e_priv *priv, int vf_number,
                            struct i40e_mbx_msg *msg)
{
    uint32_t ctl;

    if (vf_number < 0 || vf_number >= priv->num_vfs)
        return -EINVAL;

    ctl = i40e_readl(priv, I40E_VF_MBX_CTL(vf_number));
    if (!(ctl & I40E_MBX_CTL_VF2PF))
        return 0;  /* no message pending */

    msg->type  = i40e_readl(priv, I40E_VF_MBX_DATA0(vf_number));
    msg->data0 = i40e_readl(priv, I40E_VF_MBX_DATA1(vf_number));
    msg->data1 = i40e_readl(priv, I40E_VF_MBX_DATA2(vf_number));
    msg->data2 = 0;

    /* ACK by clearing the VF2PF flag */
    i40e_writel(priv, I40E_VF_MBX_CTL(vf_number), 0);
    return 1;  /* 1 message received */
}

/* ── Mailbox: PF → VF (send from PF to VF) ──────────────────────────── */

int i40e_mbx_send_to_vf(struct i40e_priv *priv, int vf_number,
                          const struct i40e_mbx_msg *msg)
{
    uint32_t ctl;

    if (vf_number < 0 || vf_number >= priv->num_vfs)
        return -EINVAL;

    ctl = i40e_readl(priv, I40E_VF_MBX_CTL(vf_number));
    if (ctl & I40E_MBX_CTL_PF2VF)
        return -EAGAIN;  /* previous PF→VF message pending */

    i40e_writel(priv, I40E_VF_MBX_DATA0(vf_number), msg->type);
    i40e_writel(priv, I40E_VF_MBX_DATA1(vf_number), msg->data0);
    i40e_writel(priv, I40E_VF_MBX_DATA2(vf_number), msg->data1);
    i40e_writel(priv, I40E_VF_MBX_CTL(vf_number),
                I40E_MBX_CTL_PF2VF);
    return 0;
}

/* ── Mailbox: PF → VF (receive on VF side) ──────────────────────────── */

int i40e_vf_mbx_recv_from_pf(struct i40e_vf_priv *priv,
                               struct i40e_mbx_msg *msg)
{
    int vf = priv->vf_number;
    uint32_t ctl;

    if (i40e_vf_shared_pf == NULL || vf < 0)
        return -ENODEV;

    ctl = i40e_readl(i40e_vf_shared_pf, I40E_VF_MBX_CTL(vf));
    if (!(ctl & I40E_MBX_CTL_PF2VF))
        return 0;  /* no message pending */

    msg->type  = i40e_readl(i40e_vf_shared_pf, I40E_VF_MBX_DATA0(vf));
    msg->data0 = i40e_readl(i40e_vf_shared_pf, I40E_VF_MBX_DATA1(vf));
    msg->data1 = i40e_readl(i40e_vf_shared_pf, I40E_VF_MBX_DATA2(vf));
    msg->data2 = 0;

    /* ACK */
    i40e_writel(i40e_vf_shared_pf, I40E_VF_MBX_CTL(vf), 0);
    return 1;
}

/* ── VF device finding ──────────────────────────────────────────────── */

/* i40e_vf_find_device - Find an i40e VF device on the PCI bus.
 *
 * Tries the standard VF device ID (0x154C) and Hyper-V variant (0x1570).
 * Returns 0 on success, -ENODEV if no VF device found. */
int i40e_vf_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(I40E_VENDOR, I40E_DEV_VF, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(I40E_VENDOR, I40E_DEV_VF_HV, pci_dev);
    if (ret >= 0)
        return 0;

    return -ENODEV;
}

/* ── VF reset ────────────────────────────────────────────────────────── */

/* i40e_vf_reset_hw - Perform a Virtual Function reset.
 *
 * Triggers a VF reset via VFGEN_RSTAT and waits for the reset to
 * complete.  Returns 0 on success, -ETIMEDOUT on timeout. */
static int i40e_vf_reset_hw(struct i40e_vf_priv *priv)
{
    uint32_t reg;
    int timeout;

    /* Initiate VF reset by writing VFR_INPROGRESS */
    i40e_vf_writel(priv, I40E_VFGEN_RSTAT, I40E_VFR_INPROGRESS);
    io_wait();

    /* Poll for reset completion */
    timeout = 500000;
    while (timeout > 0) {
        reg = i40e_vf_readl(priv, I40E_VFGEN_RSTAT);
        if ((reg & I40E_VFGEN_RSTAT_VFR_STATE) == I40E_VFR_COMPLETED)
            break;
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        kprintf("  i40e-vf: reset timeout (VFGEN_RSTAT=0x%08x)\n", reg);
        return -ETIMEDOUT;
    }

    /* Post-reset delay */
    {
        int d;
        for (d = 0; d < 10000; d++)
            io_wait();
    }

    /* Set status to VFACTIVE */
    i40e_vf_writel(priv, I40E_VFGEN_RSTAT, I40E_VFR_VFACTIVE);

    kprintf("  i40e-vf: reset completed\n");
    return 0;
}

/* ── VF TX ring setup ────────────────────────────────────────────────── */

/* i40e_vf_setup_tx_ring - Allocate and initialise a VF TX descriptor ring.
 *
 * Uses the same descriptor format and ring structure as the PF, but
 * writes to queue registers via the shared PF MMIO region.  The VF
 * uses queue indices starting at the PF's existing queues. */
static int i40e_vf_setup_tx_ring(struct i40e_vf_priv *priv, int vf_q_idx)
{
    struct i40e_txq *txq;
    uint64_t phys;
    void *virt;
    int i;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_tx_queues)
        return -EINVAL;

    /* VF queues start after PF's queues (PF uses queue 0,
     * VF uses queue Q_START + vf_q_idx). */
    hw_q_idx = I40E_MAX_TX_QUEUES + vf_q_idx;

    txq = &priv->txq[vf_q_idx];
    txq->size = I40E_TX_RING_SIZE;
    txq->next = 0;
    txq->clean = 0;

    phys = (uint64_t)pmm_alloc_frames(2);
    if (phys == 0) {
        kprintf("  i40e-vf: failed to allocate TX ring %d\n", vf_q_idx);
        return -ENOMEM;
    }

    virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)phys);
    if (virt == NULL) {
        pmm_free_frames_contiguous(phys, 2);
        return -ENOMEM;
    }

    txq->descs = (struct i40e_tx_desc *)virt;
    txq->descs_phys = phys;

    memset(txq->descs, 0, I40E_TX_RING_SIZE * sizeof(struct i40e_tx_desc));
    for (i = 0; i < I40E_TX_RING_SIZE; i++)
        txq->descs[i].olinfo_status = I40E_TXD_ST_DD;

    if (i40e_vf_shared_pf != NULL) {
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_BASE_ADDR_L(hw_q_idx),
                    (uint32_t)(phys & 0xFFFFFFFF));
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_BASE_ADDR_H(hw_q_idx),
                    (uint32_t)(phys >> 32));
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_LENGTH(hw_q_idx),
                    I40E_TX_RING_SIZE * sizeof(struct i40e_tx_desc));
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_HEAD(hw_q_idx), 0);
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_TAIL(hw_q_idx), 0);
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_ENABLED(hw_q_idx), 1);
    }

    kprintf("  i40e-vf: TX ring %d (hw_q=%d) at phys=0x%llx size=%d\n",
            vf_q_idx, hw_q_idx,
            (unsigned long long)phys, I40E_TX_RING_SIZE);
    return 0;
}

/* i40e_vf_teardown_tx_ring - Release a VF TX ring. */
static void i40e_vf_teardown_tx_ring(struct i40e_vf_priv *priv,
                                      int vf_q_idx)
{
    struct i40e_txq *txq;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_tx_queues)
        return;

    hw_q_idx = I40E_MAX_TX_QUEUES + vf_q_idx;
    txq = &priv->txq[vf_q_idx];

    if (i40e_vf_shared_pf != NULL)
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_ENABLED(hw_q_idx), 0);

    if (txq->descs_phys != 0) {
        pmm_free_frames_contiguous(txq->descs_phys, 2);
        txq->descs_phys = 0;
    }
    txq->descs = NULL;
    txq->size = 0;
}

/* ── VF RX ring setup ────────────────────────────────────────────────── */

static int i40e_vf_setup_rx_ring(struct i40e_vf_priv *priv, int vf_q_idx)
{
    struct i40e_rxq *rxq;
    uint64_t phys;
    void *virt;
    int i, page_frames;
    uint64_t pool_phys;
    void *pool_virt;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_rx_queues)
        return -EINVAL;

    hw_q_idx = I40E_MAX_RX_QUEUES + vf_q_idx;

    rxq = &priv->rxq[vf_q_idx];
    rxq->size = I40E_RX_RING_SIZE;
    rxq->next = 0;
    rxq->cur = 0;

    /* Descriptor ring — 2 pages (256 × 32 bytes) */
    phys = (uint64_t)pmm_alloc_frames(2);
    if (phys == 0) {
        kprintf("  i40e-vf: failed to allocate RX ring %d\n", vf_q_idx);
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

    /* Buffer pool */
    page_frames = I40E_RX_BUF_PAGE_COUNT;
    pool_phys = (uint64_t)pmm_alloc_frames(page_frames);
    if (pool_phys == 0) {
        pmm_free_frames_contiguous(phys, 2);
        return -ENOMEM;
    }

    pool_virt = (void *)(uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)pool_phys);
    if (pool_virt == NULL) {
        pmm_free_frames_contiguous(phys, 2);
        pmm_free_frames_contiguous(pool_phys, (size_t)page_frames);
        return -ENOMEM;
    }

    for (i = 0; i < I40E_RX_RING_SIZE; i++) {
        rxq->buffers[i] = (uint8_t *)pool_virt + i * I40E_RX_BUF_SIZE;
        rxq->buf_phys[i] = pool_phys + (uint64_t)(i * I40E_RX_BUF_SIZE);
        rxq->descs[i].addr = rxq->buf_phys[i];
        rxq->descs[i].status_error = 0;
    }

    if (i40e_vf_shared_pf != NULL) {
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_BASE_ADDR_L(hw_q_idx),
                    (uint32_t)(phys & 0xFFFFFFFF));
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_BASE_ADDR_H(hw_q_idx),
                    (uint32_t)(phys >> 32));
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_LENGTH(hw_q_idx),
                    I40E_RX_RING_SIZE * sizeof(struct i40e_rx_desc));
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_HEAD(hw_q_idx), 0);
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_TAIL(hw_q_idx),
                    I40E_RX_RING_SIZE - 1);
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_ENABLED(hw_q_idx), 1);
    }

    kprintf("  i40e-vf: RX ring %d (hw_q=%d) at phys=0x%llx "
            "buf_pool=%d pages\n",
            vf_q_idx, hw_q_idx,
            (unsigned long long)phys, page_frames);
    return 0;
}

static void i40e_vf_teardown_rx_ring(struct i40e_vf_priv *priv,
                                      int vf_q_idx)
{
    struct i40e_rxq *rxq;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_rx_queues)
        return;

    hw_q_idx = I40E_MAX_RX_QUEUES + vf_q_idx;
    rxq = &priv->rxq[vf_q_idx];

    if (i40e_vf_shared_pf != NULL)
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_ENABLED(hw_q_idx), 0);

    if (rxq->descs_phys != 0) {
        pmm_free_frames_contiguous(rxq->descs_phys, 2);
        rxq->descs_phys = 0;
    }
    rxq->descs = NULL;
    rxq->size = 0;
}

/* ── VF hardware init ────────────────────────────────────────────────── */

int i40e_vf_init_hw(struct i40e_vf_priv *priv)
{
    int ret;

    ret = i40e_vf_reset_hw(priv);
    if (ret < 0)
        return ret;

    /* Set up a single VF TX/RX queue pair */
    ret = i40e_vf_setup_tx_ring(priv, 0);
    if (ret < 0)
        return ret;

    ret = i40e_vf_setup_rx_ring(priv, 0);
    if (ret < 0) {
        i40e_vf_teardown_tx_ring(priv, 0);
        return ret;
    }

    priv->present = 1;
    kprintf("  i40e-vf: hardware initialized (1 TX/RX queue pair)\n");
    return 0;
}

void i40e_vf_shutdown_hw(struct i40e_vf_priv *priv)
{
    int q;

    if (!priv->present)
        return;

    for (q = 0; q < priv->num_tx_queues; q++)
        i40e_vf_teardown_tx_ring(priv, q);
    for (q = 0; q < priv->num_rx_queues; q++)
        i40e_vf_teardown_rx_ring(priv, q);

    priv->present = 0;
    kprintf("  i40e-vf: hardware shutdown\n");
}

/* ── VF transmit / receive (data path) ──────────────────────────────── */

static int i40e_vf_transmit_ring(struct i40e_vf_priv *priv, int vf_q_idx,
                                  const uint8_t *data, uint16_t len)
{
    struct i40e_txq *txq;
    struct i40e_tx_desc *desc;
    int idx, next;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_tx_queues)
        return -EINVAL;

    hw_q_idx = I40E_MAX_TX_QUEUES + vf_q_idx;
    txq = &priv->txq[vf_q_idx];
    idx = txq->next;

    desc = &txq->descs[idx];
    if (!(desc->olinfo_status & I40E_TXD_ST_DD)) {
        priv->tx_errors++;
        return -ENOMEM;
    }

    {
        uint64_t buf_phys;
        void *buf_virt;

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
        desc->olinfo_status = 0;
        txq->clean = idx;
    }

    next = (idx + 1) & (I40E_TX_RING_SIZE - 1);
    txq->next = next;

    if (i40e_vf_shared_pf != NULL)
        i40e_writel(i40e_vf_shared_pf, I40E_QTX_TAIL(hw_q_idx),
                    (uint32_t)next);

    priv->tx_packets++;
    priv->tx_bytes += len;
    return 0;
}

static int i40e_vf_receive_ring(struct i40e_vf_priv *priv, int vf_q_idx,
                                 uint8_t *buf, uint16_t max_len)
{
    struct i40e_rxq *rxq;
    struct i40e_rx_desc *desc;
    int idx, len = 0;
    int hw_q_idx;

    if (vf_q_idx < 0 || vf_q_idx >= priv->num_rx_queues)
        return -EINVAL;

    hw_q_idx = I40E_MAX_RX_QUEUES + vf_q_idx;
    rxq = &priv->rxq[vf_q_idx];
    idx = rxq->next;

    desc = &rxq->descs[idx];
    if (!(desc->status_error & I40E_RXD_ST_DD))
        return 0;

    if (!(desc->status_error & I40E_RXD_ST_EOP)) {
        desc->status_error = 0;
        goto skip;
    }

    len = (int)(desc->length & 0x3FFF);
    if (len > (int)max_len)
        len = (int)max_len;

    if (desc->status_error & (I40E_RXD_ERR_CE | I40E_RXD_ERR_SE |
                               I40E_RXD_ERR_RXE)) {
        priv->rx_errors++;
        goto skip;
    }

    if (len > 0 && buf != NULL)
        memcpy(buf, rxq->buffers[idx], (size_t)len);

    priv->rx_packets++;
    priv->rx_bytes += (uint64_t)len;

skip:
    desc->status_error = 0;
    desc->addr = rxq->buf_phys[idx];
    rxq->next = (idx + 1) & (I40E_RX_RING_SIZE - 1);

    if (i40e_vf_shared_pf != NULL)
        i40e_writel(i40e_vf_shared_pf, I40E_QRX_TAIL(hw_q_idx),
                    (uint32_t)rxq->next);

    return len;
}

/* ── VF netdevice callbacks ──────────────────────────────────────────── */

static int i40e_vf_netdev_transmit(struct net_device *dev,
                                    const uint8_t *data, uint16_t len)
{
    struct i40e_vf_priv *priv;

    if (dev == NULL || dev->priv == NULL)
        return -EINVAL;

    priv = (struct i40e_vf_priv *)dev->priv;
    if (!priv->present)
        return -ENODEV;

    return i40e_vf_transmit_ring(priv, 0, data, len);
}

static int i40e_vf_netdev_receive(struct net_device *dev,
                                   uint8_t *buf, uint16_t max_len)
{
    struct i40e_vf_priv *priv;

    if (dev == NULL || dev->priv == NULL)
        return -EINVAL;

    priv = (struct i40e_vf_priv *)dev->priv;
    if (!priv->present)
        return -ENODEV;

    return i40e_vf_receive_ring(priv, 0, buf, max_len);
}

/* ── VF interrupt handling ──────────────────────────────────────────── */

/* i40e_vf_irq_handler - Interrupt handler for the i40e VF.
 *
 * Shares the same interrupt resources as the PF (in the unikernel
 * model the VF doesn't have its own MSI-X vectors).  On interrupt,
 * reads VFINT_ICR0 to determine the cause.  In a real SR-IOV setup
 * the VF would have its own MSI-X table. */
void i40e_vf_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    struct i40e_vf_priv *priv;
    uint32_t icr0;

    priv = &i40e_vf_state;
    if (!priv->present)
        return;

    icr0 = i40e_vf_readl(priv, I40E_VFINT_ICR0);

    if (icr0 & I40E_ICR0_LSC)
        kprintf("  i40e-vf: link status change\n");

    if (icr0 & I40E_ICR0_RX_QUEUE) {
        /* RX data available — stack will poll via netif_recv */
    }

    if (icr0 & I40E_ICR0_TX_QUEUE) {
        /* TX completion */
        struct i40e_txq *txq = &priv->txq[0];
        struct i40e_tx_desc *desc;
        int clean_idx = txq->clean;

        desc = &txq->descs[clean_idx];
        if (desc->olinfo_status & I40E_TXD_ST_DD) {
            if (desc->addr != 0) {
                if (i40e_vf_shared_pf != NULL)
                    pmm_free_frames_contiguous(desc->addr, 1);
                desc->addr = 0;
            }
            desc->olinfo_status = I40E_TXD_ST_DD;
        }
    }

    /* Re-arm VF interrupt */
    i40e_vf_writel(priv, I40E_VFINT_DYN_CTLN(0),
                   I40E_PFINT_DYN_CTLN_INTENA |
                   (I40E_PFINT_DYN_CTLN_ITR_NONE << 3) |
                   I40E_PFINT_DYN_CTLN_INTENA_M);
}

/* ── VF MAC address (via mailbox) ────────────────────────────────────── */

/* i40e_vf_get_mac_via_mbx - Request MAC address from PF via mailbox.
 *
 * Sends a GET_MAC mailbox message to the PF and waits for a response.
 * Falls back to a locally-generated MAC if the mailbox exchange fails
 * (e.g., PF not responding). */
static void i40e_vf_get_mac_via_mbx(struct i40e_vf_priv *priv)
{
    struct i40e_mbx_msg msg, reply;
    int ret;
    int i;

    /* Try to request MAC from PF via mailbox */
    memset(&msg, 0, sizeof(msg));
    msg.type = I40E_MBX_MSG_GET_MAC;
    msg.data0 = (uint32_t)priv->vf_number;

    ret = i40e_vf_mbx_send_to_pf(priv, &msg);
    if (ret == 0) {
        /* Poll for PF response (simple spin) */
        for (i = 0; i < 1000; i++) {
            ret = i40e_vf_mbx_recv_from_pf(priv, &reply);
            if (ret > 0 && reply.type == I40E_MBX_MSG_GET_MAC) {
                /* Response contains MAC in data0/data1 */
                priv->mac[0] = (uint8_t)(reply.data0 & 0xFF);
                priv->mac[1] = (uint8_t)((reply.data0 >> 8) & 0xFF);
                priv->mac[2] = (uint8_t)((reply.data0 >> 16) & 0xFF);
                priv->mac[3] = (uint8_t)((reply.data0 >> 24) & 0xFF);
                priv->mac[4] = (uint8_t)(reply.data1 & 0xFF);
                priv->mac[5] = (uint8_t)((reply.data1 >> 8) & 0xFF);
                goto done;
            }
            io_wait();
        }
    }

    /* Fallback: generate a locally-administered MAC based on VF number */
    priv->mac[0] = 0x02;  /* locally-administered, unicast */
    priv->mac[1] = 0x00;
    priv->mac[2] = 0x1A;
    priv->mac[3] = 0x4B;
    priv->mac[4] = 0xE0;
    priv->mac[5] = (uint8_t)(0x10 + (uint8_t)priv->vf_number);

done:
    kprintf("  i40e-vf: MAC %02x:%02x:%02x:%02x:%02x:%02x%s\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5],
            ret == 0 ? " (fallback)" : "");
}

/* ── VF probe ────────────────────────────────────────────────────────── */

/* i40e_vf_probe - Probe and initialise an i40e VF device.
 *
 * In the unikernel model, the VF doesn't have its own MMIO BAR —
 * it shares the PF's MMIO mapping and uses higher queue indices.
 * This function sets up the VF state structure, obtains a MAC
 * address (via mailbox or fallback), and initialises hardware. */
int i40e_vf_probe(struct i40e_vf_priv *priv)
{
    struct pci_device pci_dev;
    int ret;

    /* Try to find a VF device on the PCI bus */
    ret = i40e_vf_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  i40e-vf: VF PCI device not found\n");
        return ret;
    }

    /* In the unikernel model, the VF shares the PF's MMIO,
     * but we record its PCI location for reference. */
    priv->mmio_phys  = 0;
    priv->mmio_base  = i40e_vf_shared_pf != NULL
                           ? i40e_vf_shared_pf->mmio_base
                           : 0;
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;
    priv->device_id  = pci_dev.device_id;
    priv->vf_number  = 0;  /* single VF for now */

    /* Record PF location for mailbox routing */
    if (i40e_vf_shared_pf != NULL) {
        /* We share the PF device — derive bus/dev/func from PF state */
        struct pci_device pf_dev;
        memset(&pf_dev, 0, sizeof(pf_dev));
        if (i40e_find_device(&pf_dev) == 0) {
            priv->pf_bus  = pf_dev.bus;
            priv->pf_dev  = pf_dev.slot;
            priv->pf_func = pf_dev.func;
        }
    }

    /* Queue counts */
    priv->num_tx_queues = 1;
    priv->num_rx_queues = 1;

    kprintf("  i40e-vf: found VF device 0x%04x at %02x:%02x.%d, "
            "PF IRQ=%d\n",
            pci_dev.device_id,
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            pci_dev.irq);

    /* Obtain MAC address (mailbox exchange with PF) */
    i40e_vf_get_mac_via_mbx(priv);

    /* Initialise hardware (reset, rings) */
    ret = i40e_vf_init_hw(priv);
    if (ret < 0)
        return ret;

    return 0;
}

/* ── VF init / exit ──────────────────────────────────────────────────── */

int i40e_vf_init(void)
{
    int ret;

    memset(&i40e_vf_state, 0, sizeof(i40e_vf_state));
    i40e_vf_state.ifindex = -1;
    i40e_vf_state.vf_number = 0;

    ret = i40e_vf_probe(&i40e_vf_state);
    if (ret < 0)
        return ret;

    /* ── Register VF netdevice ────────────────────────────────────── */
    {
        struct net_device *ndev = &i40e_vf_state.ndev;
        char name[NETDEV_NAME_MAX];
        int ifindex;

        /* Try "eth5" as VF interface (after PF's eth4) */
        snprintf(name, sizeof(name), "eth5");
        if (netif_name_to_index(name) < 0) {
            memset(ndev, 0, sizeof(*ndev));
            snprintf(ndev->name, sizeof(ndev->name), "%s", name);
            memcpy(ndev->mac, i40e_vf_state.mac, 6);
            ndev->transmit = i40e_vf_netdev_transmit;
            ndev->receive  = i40e_vf_netdev_receive;
            ndev->mtu      = 1500;
            ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev->priv     = &i40e_vf_state;

            ifindex = netif_register(ndev);
            if (ifindex >= 0) {
                i40e_vf_state.ifindex = ifindex;
                kprintf("  i40e-vf: registered as netdevice ifindex=%d "
                        "(%s)\n", ifindex, name);
            } else {
                kprintf("  i40e-vf: netdevice registration failed\n");
                i40e_vf_shutdown_hw(&i40e_vf_state);
                return -EIO;
            }
        } else {
            kprintf("  i40e-vf: %s already in use, skipping\n", name);
        }
    }

    kprintf("i40e-vf: VF driver loaded (1 TX/RX queue pair)\n");
    return 0;
}

void i40e_vf_exit(void)
{
    if (i40e_vf_state.present) {
        if (i40e_vf_state.ifindex >= 0) {
            netif_unregister(i40e_vf_state.ifindex);
            i40e_vf_state.ifindex = -1;
        }
        i40e_vf_shutdown_hw(&i40e_vf_state);
        i40e_vf_state.present = 0;
    }
    kprintf("i40e-vf: VF driver unloaded\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  SR-IOV Management (PF side)
 *
 *  Functions for enabling and managing SR-IOV Virtual Functions from
 *  the Physical Function driver.  These interact with the kernel's
 *  sriov infrastructure (sriov.c) to allocate and configure VFs.
 * ══════════════════════════════════════════════════════════════════════ */

/* i40e_sriov_configure - Enable or disable SR-IOV VFs on the i40e PF.
 *
 * When num_vfs > 0, enables the specified number of VFs.  When num_vfs
 * is 0, disables all VFs.  Returns the number of VFs enabled on success,
 * negative errno on failure. */
int i40e_sriov_configure(struct i40e_priv *priv, int num_vfs)
{
    struct pci_device pci_dev;
    int ret;

    if (num_vfs < 0)
        return -EINVAL;

    if (num_vfs == 0) {
        /* Disable all VFs */
        if (priv->num_vfs > 0) {
            memset(&pci_dev, 0, sizeof(pci_dev));
            ret = i40e_find_device(&pci_dev);
            if (ret == 0) {
                sriov_disable_vfs(pci_dev.bus, pci_dev.slot,
                                  pci_dev.func);
            }
            priv->num_vfs = 0;
            kprintf("  i40e: SR-IOV disabled (all VFs)\n");
        }
        return 0;
    }

    if (num_vfs > I40E_MAX_TX_QUEUES)
        num_vfs = I40E_MAX_TX_QUEUES;

    /* Find the PF device to access its SR-IOV capability */
    memset(&pci_dev, 0, sizeof(pci_dev));
    ret = i40e_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  i40e: cannot configure SR-IOV — PF not found\n");
        return ret;
    }

    /* Attempt SR-IOV probe and enable */
    ret = sriov_probe_pf(pci_dev.bus, pci_dev.slot, pci_dev.func);
    if (ret <= 0) {
        kprintf("  i40e: SR-IOV capability not found on PF\n");
        return -ENODEV;
    }

    /* Enable VFs through the sriov subsystem */
    ret = sriov_enable_vfs(pci_dev.bus, pci_dev.slot,
                            pci_dev.func, num_vfs);
    if (ret < 0) {
        kprintf("  i40e: sriov_enable_vfs failed (%d)\n", ret);
        return ret;
    }

    priv->num_vfs = (ret > 0) ? ret : num_vfs;

    /* Assign MAC addresses and record VF locations */
    {
        int v;
        for (v = 0; v < priv->num_vfs && v < I40E_MAX_TX_QUEUES; v++) {
            /* Generate deterministic MAC for each VF:
             * 02:00:1A:4B:E0:10 + vf_number */
            priv->vf_macs[v][0] = 0x02;
            priv->vf_macs[v][1] = 0x00;
            priv->vf_macs[v][2] = 0x1A;
            priv->vf_macs[v][3] = 0x4B;
            priv->vf_macs[v][4] = 0xE0;
            priv->vf_macs[v][5] = (uint8_t)(0x10 + (uint8_t)v);
        }
    }

    kprintf("  i40e: SR-IOV enabled with %d VF(s)\n", priv->num_vfs);
    return priv->num_vfs;
}

/* i40e_sriov_init - Initialise SR-IOV for the i40e PF (auto-probe).
 *
 * Called during i40e_init to check if the PF supports SR-IOV and
 * to report its capability.  Does not automatically enable VFs. */
int i40e_sriov_init(void)
{
    struct pci_device pci_dev;
    int ret;

    memset(&pci_dev, 0, sizeof(pci_dev));
    ret = i40e_find_device(&pci_dev);
    if (ret < 0)
        return ret;

    ret = sriov_probe_pf(pci_dev.bus, pci_dev.slot, pci_dev.func);
    if (ret > 0) {
        kprintf("  i40e: SR-IOV capable PF at %02x:%02x.%x\n",
                pci_dev.bus, pci_dev.slot, pci_dev.func);
        i40e_vf_devices_found = 1;
    }

    return ret;
}

#ifdef MODULE
#endif

module_init(i40e_init);
module_exit(i40e_exit);

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Intel Ethernet 700 Series (i40e) PF driver with SR-IOV VF support");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v00008086d00001572*");
MODULE_ALIAS("pci:v00008086d00001580*");
#endif
