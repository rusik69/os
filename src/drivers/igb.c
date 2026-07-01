/*
 * src/drivers/igb.c — Intel Gigabit Ethernet (igb) driver
 *
 * Implements a multi-queue driver for the Intel 82576 and I350
 * Gigabit Ethernet controllers.  Supports up to 4 independent
 * TX/RX queues with hardware RSS (Receive Side Scaling) and
 * per-queue interrupt assignment (IVAR).
 *
 * Task 10: igb: multi-queue support
 *   - Separate TX/RX descriptor rings per queue
 *   - RSS with Toeplitz hash and configurable redirection table
 *   - Per-queue IVAR mapping for MSI-X interrupt vectors
 *   - Queue statistics and management
 *   - Netdevice registration with per-queue selection
 */

#include "igb.h"
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
static struct igb_priv igb_state;

/* ── Forward declarations ─────────────────────────────────────────── */
static int igb_netdev_transmit(struct net_device *dev,
                               const uint8_t *data, uint16_t len);
static int igb_netdev_receive(struct net_device *dev,
                              uint8_t *buf, uint16_t max_len);

/* ── MMIO helpers (register-specific) ─────────────────────────────── */

/* Wait for the EEPROM auto-read to complete (status bit clears when done).
 * Returns 0 on success, negative errno on timeout. */
static int igb_wait_eeprom(struct igb_priv *priv)
{
    int timeout;

    timeout = 10000;
    while ((igb_readl(priv, IGB_REG_EERD) & (1U << 4)) && timeout > 0) {
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        kprintf("  igb: EEPROM auto-read timeout\n");
        return -ETIMEDOUT;
    }

    return 0;
}

/* Reset the device by setting the RST bit in CTRL, then wait for
 * the device to come out of reset.  Returns 0 on success, negative
 * errno on timeout. */
int igb_reset_hw(struct igb_priv *priv)
{
    int timeout;
    uint32_t ctrl;

    ctrl = igb_readl(priv, IGB_REG_CTRL);
    ctrl |= IGB_CTRL_RST;
    igb_writel(priv, IGB_REG_CTRL, ctrl);

    /* Wait for reset to complete — RST bit self-clears */
    timeout = 100000;
    while ((igb_readl(priv, IGB_REG_CTRL) & IGB_CTRL_RST) && timeout > 0) {
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        kprintf("  igb: device reset timeout\n");
        return -ETIMEDOUT;
    }

    /* Post-reset delay (datasheet: 1 ms minimum) */
    {
        int d;
        for (d = 0; d < 10000; d++)
            io_wait();
    }

    return 0;
}

/* ── MAC address ──────────────────────────────────────────────────── */

/* Read the MAC address from the Receive Address registers (RAL/RAH).
 * The 82576 stores the MAC in RAL0/RAH0 after reset. */
static void igb_get_mac(struct igb_priv *priv)
{
    uint32_t ral, rah;

    ral = igb_readl(priv, IGB_REG_RAL(0));
    rah = igb_readl(priv, IGB_REG_RAH(0));

    priv->mac[0] = (uint8_t)(ral & 0xFF);
    priv->mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    priv->mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    priv->mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    priv->mac[4] = (uint8_t)(rah & 0xFF);
    priv->mac[5] = (uint8_t)((rah >> 8) & 0xFF);

    kprintf("  igb: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5]);
}

/* ── TX ring setup ───────────────────────────────────────────────── */

/* Allocate and initialise a TX descriptor ring for queue @q_idx.
 * Returns 0 on success, negative errno on failure. */
int igb_setup_tx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_txq *txq;
    uint64_t phys;
    void *virt;
    int i;

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return -EINVAL;

    txq = &priv->txq[q_idx];
    txq->size = IGB_TX_RING_SIZE;
    txq->next = 0;
    txq->clean = 0;

    /* Allocate physically contiguous memory for the descriptor ring.
     * Each descriptor is 16 bytes, so ring = 256 × 16 = 4096 bytes = 1 page. */
    phys = (uint64_t)pmm_alloc_frames(1);
    if (phys == 0) {
        kprintf("  igb: failed to allocate TX ring %d\n", q_idx);
        return -ENOMEM;
    }

    virt = (void *)PHYS_TO_VIRT((void *)(uintptr_t)phys);
    memset(virt, 0, IGB_PAGE_SIZE);

    txq->descs      = (struct igb_tx_desc *)virt;
    txq->descs_phys = phys;

    /* Initialise descriptors with DD-bit cleared (driver-owned) */
    for (i = 0; i < txq->size; i++) {
        struct igb_tx_desc *d = &txq->descs[i];
        memset(d, 0, sizeof(*d));
    }

    kprintf("  igb: TX queue %d ring at phys 0x%llx (%d slots)\n",
            q_idx, (unsigned long long)phys, txq->size);

    return 0;
}

/* Free the TX descriptor ring for queue @q_idx. */
void igb_teardown_tx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_txq *txq;

    if (q_idx < 0 || q_idx >= IGB_MAX_TX_QUEUES)
        return;

    txq = &priv->txq[q_idx];
    if (txq->descs_phys != 0) {
        pmm_free_frames_contiguous(txq->descs_phys, 1);
        txq->descs      = NULL;
        txq->descs_phys = 0;
        txq->size       = 0;
    }
}

/* Program the hardware TX registers for queue @q_idx.
 * Must be called after igb_setup_tx_ring() and before enabling TX. */
static void igb_program_tx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_txq *txq;
    uint64_t phys;
    uint32_t regs[5];

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return;

    txq = &priv->txq[q_idx];
    phys = txq->descs_phys;

    regs[0] = IGB_TX_Q_REGS[q_idx][0];  /* TDBAL */
    regs[1] = IGB_TX_Q_REGS[q_idx][1];  /* TDBAH */
    regs[2] = IGB_TX_Q_REGS[q_idx][2];  /* TDLEN */
    regs[3] = IGB_TX_Q_REGS[q_idx][3];  /* TDH */
    regs[4] = IGB_TX_Q_REGS[q_idx][4];  /* TDT */

    igb_writel(priv, regs[0], (uint32_t)(phys & 0xFFFFFFFFULL));
    igb_writel(priv, regs[1], (uint32_t)(phys >> 32));
    igb_writel(priv, regs[2], (uint32_t)(txq->size * sizeof(struct igb_tx_desc)));
    igb_writel(priv, regs[3], 0);  /* TDH = 0 */
    igb_writel(priv, regs[4], 0);  /* TDT = 0 */
}

/* ── RX ring setup ────────────────────────────────────────────────── */

/* Allocate and initialise an RX descriptor ring and data buffers
 * for queue @q_idx.  Returns 0 on success, negative errno on failure. */
int igb_setup_rx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_rxq *rxq;
    uint64_t phys;
    void *virt;
    int i;

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return -EINVAL;

    rxq = &priv->rxq[q_idx];
    rxq->size = IGB_RX_RING_SIZE;
    rxq->next = 0;
    rxq->cur  = 0;

    /* Allocate physically contiguous memory for the RX descriptor ring
     * (256 × 16 = 4096 bytes = 1 page). */
    phys = (uint64_t)pmm_alloc_frames(1);
    if (phys == 0) {
        kprintf("  igb: failed to allocate RX ring %d\n", q_idx);
        return -ENOMEM;
    }

    virt = (void *)PHYS_TO_VIRT((void *)(uintptr_t)phys);
    memset(virt, 0, IGB_PAGE_SIZE);

    rxq->descs      = (struct igb_rx_desc *)virt;
    rxq->descs_phys = phys;

    /* Allocate RX data buffers from the global buffer pool.
     * The buffers are pre-allocated contiguously by igb_init_hw(). */
    for (i = 0; i < rxq->size; i++) {
        if (priv->buf_pool_virt != NULL) {
            int buf_idx = q_idx * IGB_RX_RING_SIZE + i;
            rxq->buf_phys[buf_idx] = priv->buf_pool_phys +
                (uint64_t)(buf_idx * IGB_RX_BUF_SIZE);
            rxq->buffers[i] = (uint8_t *)priv->buf_pool_virt +
                (buf_idx * IGB_RX_BUF_SIZE);
        }
    }

    /* Fill RX descriptors with buffer addresses and clear status */
    for (i = 0; i < rxq->size; i++) {
        struct igb_rx_desc *d = &rxq->descs[i];
        memset(d, 0, sizeof(*d));
        d->addr = rxq->buf_phys[i];
    }

    kprintf("  igb: RX queue %d ring at phys 0x%llx (%d slots)\n",
            q_idx, (unsigned long long)phys, rxq->size);

    return 0;
}

/* Free the RX descriptor ring for queue @q_idx. */
void igb_teardown_rx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_rxq *rxq;

    if (q_idx < 0 || q_idx >= IGB_MAX_RX_QUEUES)
        return;

    rxq = &priv->rxq[q_idx];
    if (rxq->descs_phys != 0) {
        pmm_free_frames_contiguous(rxq->descs_phys, 1);
        rxq->descs      = NULL;
        rxq->descs_phys = 0;
        rxq->size       = 0;
    }
}

/* Program the hardware RX registers for queue @q_idx. */
static void igb_program_rx_ring(struct igb_priv *priv, int q_idx)
{
    struct igb_rxq *rxq;
    uint64_t phys;
    uint32_t regs[5];

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return;

    rxq = &priv->rxq[q_idx];
    phys = rxq->descs_phys;

    regs[0] = IGB_RX_Q_REGS[q_idx][0];  /* RDBAL */
    regs[1] = IGB_RX_Q_REGS[q_idx][1];  /* RDBAH */
    regs[2] = IGB_RX_Q_REGS[q_idx][2];  /* RDLEN */
    regs[3] = IGB_RX_Q_REGS[q_idx][3];  /* RDH */
    regs[4] = IGB_RX_Q_REGS[q_idx][4];  /* RDT */

    igb_writel(priv, regs[0], (uint32_t)(phys & 0xFFFFFFFFULL));
    igb_writel(priv, regs[1], (uint32_t)(phys >> 32));
    igb_writel(priv, regs[2], (uint32_t)(rxq->size * sizeof(struct igb_rx_desc)));
    igb_writel(priv, regs[3], 0);  /* RDH = 0 */
    igb_writel(priv, regs[4], rxq->size - 1);  /* RDT = size-1 (all descs available) */
}

/* ── RSS Configuration ────────────────────────────────────────────── */

/* Set the RSS hash key (4 × 32-bit = 128 bits). */
void igb_rss_set_key(struct igb_priv *priv, const uint32_t key[4])
{
    int i;

    for (i = 0; i < 4; i++)
        igb_writel(priv, IGB_REG_RSSRK(i), key[i]);
}

/* Program the RSS redirection table (RETA).  For 4 queues we use
 * a round-robin distribution across 128 entries. */
void igb_rss_set_reta(struct igb_priv *priv)
{
    int i;
    uint32_t reta_val;

    /* RETA has 32 dword registers.  Each dword covers 4 entries
     * (8 bits per entry: bits 7:0, 15:8, 23:16, 31:24).
     * With 4 queues, each entry holds queue index 0-3. */
    for (i = 0; i < 32; i++) {
        int base = i * 4;
        uint8_t q0 = (uint8_t)(base % priv->num_rx_queues);
        uint8_t q1 = (uint8_t)((base + 1) % priv->num_rx_queues);
        uint8_t q2 = (uint8_t)((base + 2) % priv->num_rx_queues);
        uint8_t q3 = (uint8_t)((base + 3) % priv->num_rx_queues);

        reta_val = ((uint32_t)q0) |
                   ((uint32_t)q1 << 8) |
                   ((uint32_t)q2 << 16) |
                   ((uint32_t)q3 << 24);
        igb_writel(priv, IGB_REG_RETA(i), reta_val);
    }
}

/* Configure RSS: enable MRQC, set hash key, program RETA. */
void igb_rss_configure(struct igb_priv *priv)
{
    uint32_t mrqc;
    uint32_t rss_queues;

    /* Determine MRQC queue config field based on number of queues */
    if (priv->num_rx_queues <= 2)
        rss_queues = IGB_MRQC_RSS_QUEUES_2;
    else
        rss_queues = IGB_MRQC_RSS_QUEUES_4;

    /* Enable RSS with TCP/IPv4 and IPv4 hashing */
    mrqc = IGB_MRQC_RSS | IGB_MRQC_RSS_FIELD | rss_queues;
    igb_writel(priv, IGB_REG_MRQC, mrqc);

    /* Set the default RSS hash key */
    igb_rss_set_key(priv, IGB_RSS_KEY_DEFAULT);

    /* Program the redirection table */
    igb_rss_set_reta(priv);

    kprintf("  igb: RSS configured with %d RX queues\n", priv->num_rx_queues);

    /* Enable RSS hash types in MRQC — include TCP/IPv4 and IPv4 */
    mrqc |= IGB_RSS_HASH_TCP_IPV4 | IGB_RSS_HASH_IPV4;
    igb_writel(priv, IGB_REG_MRQC, mrqc);
}

/* ── IVAR (Interrupt Vector Allocation) Configuration ─────────────── */

/* Configure IVAR registers to map each TX/RX queue pair to a unique
 * MSI-X-like vector index.  Since we use legacy pin-based interrupt or
 * a single MSI vector, we map all queues to vector 0.  The IVAR is
 * still programmed so that the hardware knows which queues have
 * interrupts enabled. */
void igb_ivar_configure(struct igb_priv *priv)
{
    uint32_t ivar0;
    int q;

    ivar0 = 0;

    for (q = 0; q < priv->num_tx_queues && q < priv->num_rx_queues; q++) {
        if (q < 4) {
            /* Byte offset within IVAR0 for each queue:
             *  Queue 0: RX at byte 0, TX at byte 1
             *  Queue 1: RX at byte 2, TX at byte 3
             *  Queue 2: RX at byte 4, TX at byte 5
             *  Queue 3: RX at byte 6, TX at byte 7
             */
            uint32_t rx_shift = (uint32_t)(q * 8);       /* RX entry start */
            uint32_t tx_shift = (uint32_t)(q * 8 + 4);   /* TX entry start */

            /* Map all queues to vector 0 (we use a single interrupt) */
            ivar0 &= ~(0xFFU << rx_shift);
            ivar0 |= (IGB_IVAR_ENTRY(0) << rx_shift);

            ivar0 &= ~(0xFFU << tx_shift);
            ivar0 |= (IGB_IVAR_ENTRY(0) << tx_shift);
        }
    }

    igb_writel(priv, IGB_REG_IVAR0, ivar0);

    kprintf("  igb: IVAR configured for %d queues (all mapped to vector 0)\n",
            priv->num_tx_queues);
}

/* ── Hardware initialisation ──────────────────────────────────────── */

/* Initialise hardware: reset device, allocate buffer pool, set up
 * TX and RX rings, configure RSS and IVAR, enable transmitter
 * and receiver.  Returns 0 on success, negative errno on failure. */
int igb_init_hw(struct igb_priv *priv)
{
    uint32_t ctrl, tctl, rctl;
    uint64_t pool_phys;
    void *pool_virt;
    int q, ret;

    /* Reset the device */
    ret = igb_reset_hw(priv);
    if (ret < 0)
        return ret;

    /* Wait for EEPROM auto-read */
    ret = igb_wait_eeprom(priv);
    if (ret < 0)
        return ret;

    /* Read MAC address after reset */
    igb_get_mac(priv);

    /* Set link up (SLU bit) to force link regardless of PHY */
    ctrl = igb_readl(priv, IGB_REG_CTRL);
    ctrl |= IGB_CTRL_SLU;
    igb_writel(priv, IGB_REG_CTRL, ctrl);

    /* Allocate the RX buffer pool: contiguous memory for all RX buffers.
     * Total = num_rx_queues * ring_size * buf_size */
    {
        int total_bufs;
        int total_pages;

        total_bufs  = priv->num_rx_queues * IGB_RX_RING_SIZE;
        total_pages = (total_bufs * IGB_RX_BUF_SIZE + IGB_PAGE_SIZE - 1)
                      / IGB_PAGE_SIZE;

        pool_phys = (uint64_t)pmm_alloc_frames(total_pages);
        if (pool_phys == 0) {
            kprintf("  igb: failed to allocate RX buffer pool (%d pages)\n",
                    total_pages);
            return -ENOMEM;
        }

        pool_virt = (void *)PHYS_TO_VIRT((void *)(uintptr_t)pool_phys);
        memset(pool_virt, 0, (size_t)total_pages * IGB_PAGE_SIZE);

        priv->buf_pool_phys = pool_phys;
        priv->buf_pool_virt = pool_virt;

        kprintf("  igb: RX buffer pool at phys 0x%llx (%d pages, %d bufs)\n",
                (unsigned long long)pool_phys, total_pages, total_bufs);
    }

    /* Set up TX rings */
    for (q = 0; q < priv->num_tx_queues; q++) {
        ret = igb_setup_tx_ring(priv, q);
        if (ret < 0)
            goto err_teardown_tx;

        igb_program_tx_ring(priv, q);
    }

    /* Set up RX rings */
    for (q = 0; q < priv->num_rx_queues; q++) {
        ret = igb_setup_rx_ring(priv, q);
        if (ret < 0)
            goto err_teardown_rx;

        igb_program_rx_ring(priv, q);
    }

    /* Configure RSS for multi-queue RX distribution */
    igb_rss_configure(priv);

    /* Configure IVAR for per-queue interrupt assignment */
    igb_ivar_configure(priv);

    /* ── Enable transmitter ──────────────────────────────────────── */
    tctl = igb_readl(priv, IGB_REG_TCTL);
    tctl |= IGB_TCTL_EN;        /* Transmitter Enable */
    tctl |= IGB_TCTL_PSP;       /* Pad Short Packets */
    tctl |= (0x0F << IGB_TCTL_CT_SHIFT);   /* Collision Threshold = 15 */
    tctl |= (0x3F << IGB_TCTL_COLD_SHIFT); /* Collision Distance = 63 */
    igb_writel(priv, IGB_REG_TCTL, tctl);

    /* ── Enable receiver ──────────────────────────────────────────── */
    rctl = igb_readl(priv, IGB_REG_RCTL);
    rctl |= IGB_RCTL_EN;         /* Receiver Enable */
    rctl |= IGB_RCTL_BAM;        /* Accept Broadcast */
    rctl |= IGB_RCTL_SECRC;      /* Strip CRC */
    rctl |= IGB_RCTL_MQ;         /* Multiple Queues (RSS) */
    rctl &= ~IGB_RCTL_SZ_2048;   /* Buffer size = 2048 (default) */
    igb_writel(priv, IGB_REG_RCTL, rctl);

    /* ── Enable interrupts (global) ───────────────────────────────── */
    igb_writel(priv, IGB_REG_IMS,
               (1U << 0)  |  /* TX Queue 0 */
               (1U << 1)  |  /* TX Queue 1 */
               (1U << 2)  |  /* TX Queue 2 */
               (1U << 3)  |  /* TX Queue 3 */ 
               (1U << 4)  |  /* RX Queue 0 */
               (1U << 5)  |  /* RX Queue 1 */
               (1U << 6)  |  /* RX Queue 2 */
               (1U << 7)  |  /* RX Queue 3 */
               (1U << 15) |  /* Link Status Change */
               (1U << 21));  /* Receive Timer Interrupt */

    /* Clear any pending interrupts */
    (void)igb_readl(priv, IGB_REG_ICR);

    kprintf("  igb: hardware initialised (%d TX, %d RX queues)\n",
            priv->num_tx_queues, priv->num_rx_queues);

    return 0;

err_teardown_rx:
    /* Tear down any RX rings that were set up */
    for (q = 0; q < priv->num_rx_queues; q++) {
        if (priv->rxq[q].descs_phys != 0)
            igb_teardown_rx_ring(priv, q);
    }

err_teardown_tx:
    /* Tear down any TX rings that were set up */
    for (q = 0; q < priv->num_tx_queues; q++) {
        if (priv->txq[q].descs_phys != 0)
            igb_teardown_tx_ring(priv, q);
    }

    /* Free buffer pool */
    if (priv->buf_pool_phys != 0) {
        int total_pages;
        int total_bufs = priv->num_rx_queues * IGB_RX_RING_SIZE;
        total_pages = (total_bufs * IGB_RX_BUF_SIZE + IGB_PAGE_SIZE - 1)
                      / IGB_PAGE_SIZE;
        pmm_free_frames_contiguous(priv->buf_pool_phys, (size_t)total_pages);
        priv->buf_pool_phys = 0;
        priv->buf_pool_virt = NULL;
    }

    return ret;
}

/* Shut down hardware: disable TX/RX, disable interrupts, reset. */
void igb_shutdown_hw(struct igb_priv *priv)
{
    int q;

    /* Disable all interrupts */
    igb_writel(priv, IGB_REG_IMC, 0xFFFFFFFFU);

    /* Disable transmitter and receiver */
    igb_writel(priv, IGB_REG_TCTL, 0);
    igb_writel(priv, IGB_REG_RCTL, 0);

    /* Tear down rings */
    for (q = 0; q < IGB_MAX_TX_QUEUES; q++)
        igb_teardown_tx_ring(priv, q);

    for (q = 0; q < IGB_MAX_RX_QUEUES; q++)
        igb_teardown_rx_ring(priv, q);

    /* Free the RX buffer pool */
    if (priv->buf_pool_phys != 0) {
        int total_bufs = priv->num_rx_queues * IGB_RX_RING_SIZE;
        int total_pages = (total_bufs * IGB_RX_BUF_SIZE + IGB_PAGE_SIZE - 1)
                          / IGB_PAGE_SIZE;
        pmm_free_frames_contiguous(priv->buf_pool_phys, (size_t)total_pages);
        priv->buf_pool_phys = 0;
        priv->buf_pool_virt = NULL;
    }

    /* Reset the device */
    igb_reset_hw(priv);

    kprintf("  igb: hardware shut down\n");
}

/* ── Data path: transmit ──────────────────────────────────────────── */

/* Transmit a packet on queue @q_idx.
 * The caller provides the complete Ethernet frame in @data.
 * Returns 0 on success, NETDEV_TX_BUSY if ring is full,
 * negative errno on other errors. */
int igb_transmit_ring(struct igb_priv *priv, int q_idx,
                      const uint8_t *data, uint16_t len)
{
    struct igb_txq *txq;
    struct igb_tx_desc *desc;
    int slot;

    if (!priv->present)
        return -EIO;

    if (q_idx < 0 || q_idx >= priv->num_tx_queues)
        return -EINVAL;

    if (len == 0)
        return -EINVAL;

    txq = &priv->txq[q_idx];
    slot = txq->next;

    /* Check whether the next descriptor is still owned by the device.
     * The DD bit is set by the device when it has finished DMA
     * (i.e., the descriptor is back to driver ownership). */
    desc = &txq->descs[slot];
    if (!(desc->status & IGB_TXD_ST_DD)) {
        /* Descriptor is still in use by device — ring full or stalled */
        priv->tx_errors++;
        return NETDEV_TX_BUSY;
    }

    /* Fill the TX descriptor */
    memset(desc, 0, sizeof(*desc));
    desc->addr   = VIRT_TO_PHYS((void *)(uintptr_t)data);
    desc->length = len;
    desc->cmd    = IGB_TXD_CMD_EOP |    /* End of Packet */
                   IGB_TXD_CMD_IFCS |   /* Insert FCS */
                   IGB_TXD_CMD_RS;      /* Report Status */

    /* Ensure descriptor writes are visible before updating TDT */
    wmb();

    /* Advance producer index */
    txq->next = (slot + 1) % txq->size;

    /* Write the new tail pointer to kick the device */
    igb_writel(priv, IGB_TX_Q_REGS[q_idx][4], (uint32_t)txq->next);

    priv->tx_packets++;
    priv->tx_bytes += len;

    return 0;
}

/* Select TX queue based on a simple hash of the destination MAC.
 * This gives a basic per-flow queue distribution when RSS is not
 * used on the TX path. */
static int igb_select_tx_queue(struct igb_priv *priv,
                               const uint8_t *data)
{
    uint32_t hash;
    int i;

    if (priv->num_tx_queues <= 1)
        return 0;

    /* Simple hash over the Ethernet destination MAC (first 6 bytes) */
    hash = 0;
    for (i = 0; i < 6; i++)
        hash = (hash << 5) - hash + (uint32_t)data[i];

    return (int)(hash % (uint32_t)priv->num_tx_queues);
}

/* ── Data path: receive ───────────────────────────────────────────── */

/* Poll for received packets on queue @q_idx.
 * Copies up to @max_len bytes of the next received frame into @buf.
 * Returns the number of bytes received, 0 if nothing available,
 * or negative errno on error. */
int igb_receive_ring(struct igb_priv *priv, int q_idx,
                     uint8_t *buf, uint16_t max_len)
{
    struct igb_rxq *rxq;
    struct igb_rx_desc *desc;
    int slot;
    uint16_t pkt_len;

    pkt_len = 0;

    if (!priv->present)
        return -EIO;

    if (q_idx < 0 || q_idx >= priv->num_rx_queues)
        return -EINVAL;

    rxq = &priv->rxq[q_idx];
    slot = rxq->cur;
    desc = &rxq->descs[slot];

    /* Check if descriptor is done (DD bit set by device) */
    if (!(desc->status & IGB_RXD_ST_DD))
        return 0;  /* Nothing available */

    /* Check for errors */
    if (desc->errors != 0) {
        priv->rx_errors++;
        goto skip_packet;
    }

    /* Read the packet length */
    if (!(desc->status & IGB_RXD_ST_EOP)) {
        /* Multi-descriptor packet — not handled in this simple driver.
         * Skip it (the data is probably fragmented across descriptors). */
        priv->rx_errors++;
        goto skip_packet;
    }

    pkt_len = desc->length;
    if (pkt_len > max_len)
        pkt_len = max_len;

    /* Copy the received data from the buffer */
    memcpy(buf, rxq->buffers[slot], pkt_len);

    priv->rx_packets++;
    priv->rx_bytes += pkt_len;

skip_packet:
    /* Refill the descriptor with the buffer address */
    memset(desc, 0, sizeof(*desc));
    desc->addr = rxq->buf_phys[slot];

    /* Advance the consumer index */
    rxq->cur = (slot + 1) % rxq->size;

    /* Update RDT to tell the device more descriptors are available.
     * RDT is the tail: device owns descriptors < RDT, driver owns >= RDT.
     * We set RDT to the last descriptor we refilled (cur-1 modulo size). */
    {
        int new_rdt = (rxq->cur - 1 + rxq->size) % rxq->size;
        igb_writel(priv, IGB_RX_Q_REGS[q_idx][4], (uint32_t)new_rdt);
    }

    if (desc->errors != 0)
        return 0;

    return (int)pkt_len;
}

/* Select RX queue for polling — round-robin across available queues. */
static int igb_select_rx_queue(struct igb_priv *priv)
{
    /* In a polled driver, we just check all queues.
     * The netdevice layer calls receive once per poll, so we
     * return the RX queue that has data.  For simplicity, we
     * start with queue 0 and check sequentially. */
    return 0;
}

/* ── Netdevice callbacks ──────────────────────────────────────────── */

static int igb_netdev_transmit(struct net_device *dev,
                               const uint8_t *data, uint16_t len)
{
    struct igb_priv *priv;
    int q_idx;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct igb_priv *)dev->priv;

    /* Select TX queue based on destination MAC hash */
    q_idx = igb_select_tx_queue(priv, data);

    return igb_transmit_ring(priv, q_idx, data, len);
}

static int igb_netdev_receive(struct net_device *dev,
                              uint8_t *buf, uint16_t max_len)
{
    struct igb_priv *priv;
    int q, received;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct igb_priv *)dev->priv;
    received = 0;

    /* Poll all RX queues for available packets */
    for (q = 0; q < priv->num_rx_queues; q++) {
        int ret;

        ret = igb_receive_ring(priv, q, buf, max_len);
        if (ret > 0) {
            received = ret;
            break;
        }
    }

    return received;
}

/* ── Statistics ───────────────────────────────────────────────────── */

void igb_print_stats(struct igb_priv *priv)
{
    kprintf("  igb: TX p=%llu b=%llu err=%llu | RX p=%llu b=%llu err=%llu\n",
            (unsigned long long)priv->tx_packets,
            (unsigned long long)priv->tx_bytes,
            (unsigned long long)priv->tx_errors,
            (unsigned long long)priv->rx_packets,
            (unsigned long long)priv->rx_bytes,
            (unsigned long long)priv->rx_errors);
}

/* ── PCI probe ────────────────────────────────────────────────────── */

int igb_find_device(struct pci_device *pci_dev)
{
    int ret;

    /* Try 82576 first, then I350, then I210 */
    ret = pci_find_device(IGB_VENDOR, IGB_DEV_82576, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(IGB_VENDOR, IGB_DEV_I350, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(IGB_VENDOR, IGB_DEV_I210, pci_dev);
    if (ret >= 0)
        return 0;

    return -ENODEV;
}

int igb_probe(struct igb_priv *priv)
{
    struct pci_device pci_dev;
    uint64_t mmio_phys;
    int ret;

    /* Find an igb-compatible device on the PCI bus */
    ret = igb_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  igb: device not found\n");
        return ret;
    }

    /* BAR0 contains the MMIO base address */
    mmio_phys = (uint64_t)(pci_dev.bar[0] & ~0xFULL);
    if (mmio_phys == 0) {
        kprintf("  igb: invalid MMIO base from BAR0\n");
        return -ENODEV;
    }

    priv->mmio_phys  = mmio_phys;
    priv->mmio_base  = (uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)mmio_phys);
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;
    priv->device_id  = pci_dev.device_id;

    /* Determine queue count based on device */
    if (pci_dev.device_id == IGB_DEV_I350) {
        /* I350 supports up to 8 queues; use 4 for compatibility */
        priv->num_tx_queues = 4;
        priv->num_rx_queues = 4;
    } else {
        /* 82576 and I210 support up to 4 queues */
        priv->num_tx_queues = 4;
        priv->num_rx_queues = 4;
    }

    /* Enable PCI bus mastering for DMA */
    pci_enable_bus_master(&pci_dev);

    kprintf("  igb: found 0x%04x at %02x:%02x.%d, "
            "MMIO phys 0x%llx (virt 0x%lx), IRQ %d, %dTX/%dRX queues\n",
            (unsigned)pci_dev.device_id,
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            (unsigned long long)priv->mmio_phys,
            (unsigned long)priv->mmio_base,
            priv->irq_line,
            priv->num_tx_queues, priv->num_rx_queues);

    /* Initialise hardware (rings, RSS, IVAR) */
    ret = igb_init_hw(priv);
    if (ret < 0) {
        kprintf("  igb: hardware initialisation failed (%d)\n", ret);
        return ret;
    }

    priv->present = 1;
    return 0;
}

/* ── Module / device init entry point ─────────────────────────────── */

int igb_init(void)
{
    int ret;

    memset(&igb_state, 0, sizeof(igb_state));
    igb_state.ifindex = -1;

    ret = igb_probe(&igb_state);
    if (ret < 0)
        return ret;

    /* ── Register with netdevice layer ───────────────────────────── */
    {
        struct net_device *ndev = &igb_state.ndev;

        /* Try eth2 (after e1000=eth0, vmxnet3=eth1, rtl8139=eth2) */
        if (netif_name_to_index("eth3") < 0) {
            memset(ndev, 0, sizeof(*ndev));
            snprintf(ndev->name, sizeof(ndev->name), "eth3");
            memcpy(ndev->mac, igb_state.mac, 6);
            ndev->transmit = igb_netdev_transmit;
            ndev->receive  = igb_netdev_receive;
            ndev->mtu      = 1500;
            ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev->priv     = &igb_state;

            int ifindex = netif_register(ndev);
            if (ifindex >= 0) {
                igb_state.ifindex = ifindex;
                kprintf("  igb: registered as netdevice ifindex=%d\n",
                        ifindex);
            } else {
                kprintf("  igb: netdevice registration failed\n");
                igb_shutdown_hw(&igb_state);
                return -EIO;
            }
        } else {
            /* eth3 already taken — try next available */
            char name[NETDEV_NAME_MAX];
            int i;

            for (i = 3; i < NETDEV_MAX; i++) {
                snprintf(name, sizeof(name), "eth%d", i);
                if (netif_name_to_index(name) < 0) {
                    memset(ndev, 0, sizeof(*ndev));
                    snprintf(ndev->name, sizeof(ndev->name), "%s", name);
                    memcpy(ndev->mac, igb_state.mac, 6);
                    ndev->transmit = igb_netdev_transmit;
                    ndev->receive  = igb_netdev_receive;
                    ndev->mtu      = 1500;
                    ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
                    ndev->priv     = &igb_state;

                    int ifindex = netif_register(ndev);
                    if (ifindex >= 0) {
                        igb_state.ifindex = ifindex;
                        kprintf("  igb: registered as netdevice ifindex=%d (%s)\n",
                                ifindex, name);
                        break;
                    }
                }
            }

            if (igb_state.ifindex < 0) {
                kprintf("  igb: no available netdevice slot\n");
                igb_shutdown_hw(&igb_state);
                return -ENOSPC;
            }
        }
    }

    kprintf("igb: driver loaded with multi-queue (%d TX, %d RX queues)\n",
            igb_state.num_tx_queues, igb_state.num_rx_queues);
    return 0;
}

void igb_exit(void)
{
    if (igb_state.present) {
        /* Unregister from netdevice layer */
        if (igb_state.ifindex >= 0) {
            netif_unregister(igb_state.ifindex);
            igb_state.ifindex = -1;
        }

        /* Shut down hardware */
        igb_shutdown_hw(&igb_state);

        igb_state.present = 0;
    }
    kprintf("igb: driver unloaded\n");
}

#ifdef MODULE
module_init(igb_init);
module_exit(igb_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Intel Gigabit Ethernet (igb) multi-queue driver (82576/I350)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v00008086d000010C9*");
MODULE_ALIAS("pci:v00008086d00001521*");
MODULE_ALIAS("pci:v00008086d00001533*");
#endif
