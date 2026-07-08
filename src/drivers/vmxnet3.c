/*
 * src/drivers/vmxnet3.c — VMware vmxnet3 Ethernet driver
 *
 * Implements the VMware vmxnet3 paravirtualized NIC.
 * Uses PCI vendor 0x15AD, device 0x07B0.
 * Provides MMIO-based register access, descriptor-ring TX/RX,
 * and netdevice registration.
 *
 * Task 8: PCI device identification
 * Task 9: TX/RX descriptor rings
 */

#include "vmxnet3.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "netdevice.h"
#include "pmm.h"
#include "errno.h"

#include "module.h"
#ifdef MODULE
#endif

/* ── Static driver state ─────────────────────────────────────────── */
static struct vmxnet3_priv vmxnet3_state;

/* ── Forward declarations ────────────────────────────────────────── */
static int vmxnet3_netdev_transmit(struct net_device *dev,
                                   const uint8_t *data, uint16_t len);
static int vmxnet3_netdev_receive(struct net_device *dev,
                                  uint8_t *buf, uint16_t max_len);
static void vmxnet3_get_mac(struct vmxnet3_priv *priv);
static int  vmxnet3_cmd(struct vmxnet3_priv *priv, uint32_t cmd,
                        uint32_t arg);

/* ── MMIO command helper ─────────────────────────────────────────── */

/* Issue a command to the device and wait for completion.
 * Returns 0 on success, negative errno on timeout. */
static int vmxnet3_cmd(struct vmxnet3_priv *priv, uint32_t cmd,
                       uint32_t arg)
{
    int timeout;
    uint32_t ecr;

    /* Write the command argument first (some commands use it) */
    if (cmd == VMXNET3_CMD_ACTIVATE || cmd == VMXNET3_CMD_DEACTIVATE) {
        /* Activation commands use DSAL/DSAH as argument */
        vmxnet3_writel(priv, VMXNET3_REG_DSAL,
                       (uint32_t)(priv->shared_phys & 0xFFFFFFFFULL));
        vmxnet3_writel(priv, VMXNET3_REG_DSAH,
                       (uint32_t)(priv->shared_phys >> 32));
    }

    /* Write the command to the CMD register */
    vmxnet3_writel(priv, VMXNET3_REG_CMD, cmd);

    /* Wait for the command to complete — the device sets ECR bit 0
     * when the command is done. */
    timeout = 1000000;
    while (timeout > 0) {
        ecr = vmxnet3_readl(priv, VMXNET3_REG_ECR);
        if (ecr & 1U) {
            /* Clear the command-done bit by writing it back */
            vmxnet3_writel(priv, VMXNET3_REG_ECR, ecr & ~1U);
            break;
        }
        io_wait();
        timeout--;
    }

    if (timeout <= 0) {
        kprintf("  vmxnet3: command 0x%x timed out\n", (unsigned)cmd);
        return -ETIMEDOUT;
    }

    return 0;
}

/* ── MAC address access ──────────────────────────────────────────── */

/* Read the MAC address from the device's MACL/MACH registers. */
static void vmxnet3_get_mac(struct vmxnet3_priv *priv)
{
    uint32_t macl, mach;

    /* The MAC address registers are readable only after a successful
     * GET_MAC command.  We issue it to make sure they are populated. */
    (void)vmxnet3_cmd(priv, VMXNET3_CMD_GET_MAC, 0);

    macl = vmxnet3_readl(priv, VMXNET3_REG_MACL);
    mach = vmxnet3_readl(priv, VMXNET3_REG_MACH);

    priv->mac[0] = (uint8_t)(macl & 0xFF);
    priv->mac[1] = (uint8_t)((macl >> 8) & 0xFF);
    priv->mac[2] = (uint8_t)((macl >> 16) & 0xFF);
    priv->mac[3] = (uint8_t)((macl >> 24) & 0xFF);
    priv->mac[4] = (uint8_t)(mach & 0xFF);
    priv->mac[5] = (uint8_t)((mach >> 8) & 0xFF);

    kprintf("  vmxnet3: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5]);
}

/* ── Shared memory / ring setup ──────────────────────────────────── */

/* Allocate the physically-contiguous shared memory region and set up
 * the driver-shared structure with ring addresses.
 *
 * Layout within the allocated pages:
 *   0x0000  – driver_shared
 *   0x1000  – TX descriptor ring
 *   0x1400  – TX completion ring
 *   0x1800  – RX descriptor ring
 *   0x1C00  – RX completion ring
 *   0x2000  – RX data buffers (64 × 2048 = 128 KB)
 */
int vmxnet3_setup_rings(struct vmxnet3_priv *priv)
{
    uint64_t shared_phys;
    struct vmxnet3_driver_shared *ds;
    void *shared_virt;
    int i;

    /* Allocate physically contiguous memory */
    shared_phys = (uint64_t)pmm_alloc_frames(VMXNET3_SHARED_PAGES);
    if (shared_phys == 0) {
        kprintf("  vmxnet3: failed to allocate %d pages for shared region\n",
                VMXNET3_SHARED_PAGES);
        return -ENOMEM;
    }

    /* Map to virtual address */
    shared_virt = (void *)PHYS_TO_VIRT((void *)(uintptr_t)shared_phys);
    memset(shared_virt, 0, VMXNET3_SHARED_SIZE);

    priv->shared_phys  = shared_phys;
    priv->shared_virt  = shared_virt;
    priv->shared_pages = VMXNET3_SHARED_PAGES;

    /* Initialise with a single TX queue and a single RX queue */
    priv->num_tx_queues = 1;
    priv->num_rx_queues = 1;

    /* ── Set up TX queue 0 ─────────────────────────────────────────── */
    {
        struct vmxnet3_txq *txq = &priv->txq[0];

        txq->descs_phys = shared_phys + VMXNET3_TX_RING_OFFSET;
        txq->comp_phys  = shared_phys + VMXNET3_TX_COMP_OFFSET;
        txq->descs = (struct vmxnet3_tx_desc *)
            ((uint8_t *)shared_virt + VMXNET3_TX_RING_OFFSET);
        txq->comp  = (struct vmxnet3_tx_comp *)
            ((uint8_t *)shared_virt + VMXNET3_TX_COMP_OFFSET);
        txq->size      = VMXNET3_TX_RING_SIZE;
        txq->gen       = 0;
        txq->comp_gen  = 0;
        txq->next      = 0;
        txq->comp_next = 0;

        /* Initialise TX descriptors — set gen to 0 (driver owned),
         * clear all other fields. */
        for (i = 0; i < txq->size; i++) {
            struct vmxnet3_tx_desc *d = &txq->descs[i];
            memset(d, 0, sizeof(*d));
            d->gen = (unsigned int)(txq->gen ? 1 : 0);
        }
    }

    /* ── Set up RX queue 0 ─────────────────────────────────────────── */
    {
        struct vmxnet3_rxq *rxq = &priv->rxq[0];
        uint64_t buf_area_phys;
        uint8_t *buf_area_virt;

        rxq->descs_phys = shared_phys + VMXNET3_RX_RING_OFFSET;
        rxq->comp_phys  = shared_phys + VMXNET3_RX_COMP_OFFSET;
        rxq->descs = (struct vmxnet3_rx_desc *)
            ((uint8_t *)shared_virt + VMXNET3_RX_RING_OFFSET);
        rxq->comp  = (struct vmxnet3_rx_comp *)
            ((uint8_t *)shared_virt + VMXNET3_RX_COMP_OFFSET);
        rxq->size      = VMXNET3_RX_RING_SIZE;
        rxq->gen       = 0;
        rxq->comp_gen  = 0;
        rxq->next      = 0;
        rxq->comp_next = 0;

        /* RX data buffers live after the completion ring in the shared
         * memory region.  We carve them from the same allocation. */
        buf_area_phys = shared_phys + VMXNET3_RX_BUF_OFFSET;
        buf_area_virt = (uint8_t *)shared_virt + VMXNET3_RX_BUF_OFFSET;

        for (i = 0; i < rxq->size; i++) {
            rxq->buf_phys[i] = buf_area_phys + (uint64_t)(i * VMXNET3_RX_BUF_SIZE);
            rxq->buffers[i]  = buf_area_virt + (i * VMXNET3_RX_BUF_SIZE);
        }

        /* Fill RX descriptors with buffer addresses and set gen = 0
         * (driver owned).  The device may begin writing to buffers
         * whose gen matches its current generation. */
        for (i = 0; i < rxq->size; i++) {
            struct vmxnet3_rx_desc *d = &rxq->descs[i];
            memset(d, 0, sizeof(*d));
            d->addr = rxq->buf_phys[i];
            d->len  = VMXNET3_RX_BUF_SIZE;
            d->btype = 0;   /* single-buffer mode */
            d->gen   = (unsigned int)(rxq->gen ? 1 : 0);
        }
    }

    /* ── Fill the driver-shared structure ──────────────────────────── */
    ds = (struct vmxnet3_driver_shared *)shared_virt;

    ds->tx_num_queues = (uint32_t)priv->num_tx_queues;
    ds->rx_num_queues = (uint32_t)priv->num_rx_queues;

    ds->tx_ring_phys[0] = priv->txq[0].descs_phys;
    ds->rx_ring_phys[0] = priv->rxq[0].descs_phys;
    ds->tx_comp_phys[0] = priv->txq[0].comp_phys;
    ds->rx_comp_phys[0] = priv->rxq[0].comp_phys;

    ds->tx_ring_size = VMXNET3_TX_RING_SIZE;
    ds->rx_ring_size = VMXNET3_RX_RING_SIZE;
    ds->tx_comp_size = VMXNET3_TX_COMP_SIZE;
    ds->rx_comp_size = VMXNET3_RX_COMP_SIZE;

    kprintf("  vmxnet3: shared region at phys 0x%llx (%d pages)\n",
            (unsigned long long)shared_phys, VMXNET3_SHARED_PAGES);
    kprintf("  vmxnet3: TX ring=%d slots, RX ring=%d slots, "
            "RX buf=%d bytes\n",
            VMXNET3_TX_RING_SIZE, VMXNET3_RX_RING_SIZE,
            VMXNET3_RX_BUF_SIZE);

    return 0;
}

/* Free the shared memory region previously allocated by setup_rings. */
void vmxnet3_teardown_rings(struct vmxnet3_priv *priv)
{
    if (priv->shared_phys != 0 && priv->shared_pages > 0) {
        pmm_free_frames_contiguous(priv->shared_phys,
                                   (size_t)priv->shared_pages);
        priv->shared_phys = 0;
        priv->shared_virt = NULL;
        priv->shared_pages = 0;
    }
}

/* ── Device activation ───────────────────────────────────────────── */

/* Activate the device — tell it where the shared memory region is,
 * then send the ACTIVATE command. */
int vmxnet3_activate(struct vmxnet3_priv *priv)
{
    int ret;

    ret = vmxnet3_cmd(priv, VMXNET3_CMD_ACTIVATE, 0);
    if (ret < 0)
        return ret;

    kprintf("  vmxnet3: device activated\n");
    return 0;
}

/* Deactivate the device — quiesce rings and stop DMA. */
void vmxnet3_deactivate(struct vmxnet3_priv *priv)
{
    /* Quiesce the device first (stops DMA without resetting) */
    (void)vmxnet3_cmd(priv, VMXNET3_CMD_QUIESCE, 0);

    /* Then deactivate */
    (void)vmxnet3_cmd(priv, VMXNET3_CMD_DEACTIVATE, 0);

    kprintf("  vmxnet3: device deactivated\n");
}

/* ── Data path: transmit ─────────────────────────────────────────── */

/* Transmit a packet through the vmxnet3 TX ring.
 *
 * The caller provides the complete Ethernet frame in @data (len bytes).
 * We place it in the next available TX descriptor slot.  The device
 * DMA-reads from the caller's buffer, so we point the descriptor at
 * the caller's data (no copy).
 *
 * Returns 0 on success, negative errno on failure. */
int vmxnet3_transmit(struct vmxnet3_priv *priv,
                     const uint8_t *data, uint16_t len)
{
    struct vmxnet3_txq *txq;
    struct vmxnet3_tx_desc *desc;
    int slot;

    if (!priv->present)
        return -EIO;

    if (len == 0)
        return -EINVAL;

    txq = &priv->txq[0];
    slot = txq->next;

    /* Check whether the next descriptor is still owned by the device
     * (i.e., the ring is full).  We check the TX completion ring to
     * see if the device has finished with any descriptors. */
    {
        struct vmxnet3_tx_comp *comp;

        /* Process any pending completions first to free slots */
        comp = &txq->comp[txq->comp_next];
        while (comp->gen == txq->comp_gen) {
            txq->comp_next = (txq->comp_next + 1) % txq->size;
            if (txq->comp_next == 0)
                txq->comp_gen ^= 1;

            priv->tx_packets++;
            comp = &txq->comp[txq->comp_next];
        }

        /* After processing completions, re-check if our slot is free.
         * The descriptor is still owned by the device if its gen bit
         * has been toggled by the device (i.e., gen != txq->gen means
         * device-owned). */
        desc = &txq->descs[slot];
        if (desc->gen != txq->gen) {
            /* Ring is still full after processing completions */
            priv->tx_errors++;
            return NETDEV_TX_BUSY;
        }
    }

    /* Fill the TX descriptor — point it at the caller's data */
    desc = &txq->descs[slot];
    memset(desc, 0, sizeof(*desc));
    desc->addr = VIRT_TO_PHYS((void *)(uintptr_t)data);
    desc->len  = (uint32_t)len;
    desc->eop  = 1;               /* end of packet (single-fragment frame) */
    desc->cq   = 1;               /* request completion notification */
    desc->gen  = (unsigned int)(txq->gen ? 1 : 0);        /* current generation = driver owned */

    /* Advance the producer index */
    txq->next = (slot + 1) % txq->size;
    if (txq->next == 0)
        txq->gen ^= 1;            /* toggle generation at wrap */

    /* Signal the device that new TX descriptors are available.
     * The device monitors the TX descriptor ring's gen bit and
     * processes descriptors whose gen matches its current generation.
     * We kick it by writing a "TX doorbell" via the ICR register
     * (which is read/write — writing bits acknowledges interrupts
     * on real hardware, but the device watches for new descriptors
     * by periodically scanning the ring).  If the device polls the
     * ring, no doorbell is needed; if it's interrupt-driven, the
     * doorbell helps.  We use CMD as a kick mechanism. */
    vmxnet3_writel(priv, VMXNET3_REG_ECR, VMXNET3_ICR_TXDONE0);

    priv->tx_bytes += len;
    return 0;
}

/* ── Data path: receive ──────────────────────────────────────────── */

/* Poll for a received packet on the vmxnet3 RX ring.
 *
 * Copies the received Ethernet frame into @buf (max @max_len bytes).
 * Returns the number of bytes received, 0 if nothing available, or
 * negative errno on error. */
int vmxnet3_receive(struct vmxnet3_priv *priv,
                    uint8_t *buf, uint16_t max_len)
{
    struct vmxnet3_rxq *rxq;
    struct vmxnet3_rx_comp *comp;
    int received;

    if (!priv->present)
        return -EIO;

    rxq = &priv->rxq[0];
    received = 0;

    /* Check the RX completion ring for new packets */
    comp = &rxq->comp[rxq->comp_next];

    /* The device writes completion entries with gen toggled.
     * Our comp_gen tracks the last gen we consumed. */
    while (comp->gen == rxq->comp_gen) {
        int comp_slot;
        int buf_idx;
        uint16_t pkt_len;

        comp_slot = rxq->comp_next;
        buf_idx   = comp_slot;  /* one-to-one mapping in single-buf mode */

        pkt_len = (uint16_t)(comp->len & 0xFFFF);
        if (pkt_len > VMXNET3_RX_BUF_SIZE)
            pkt_len = VMXNET3_RX_BUF_SIZE;

        if (pkt_len > 0 && comp->eop) {
            /* Valid packet — copy the data from the buffer */
            uint16_t copy_len = pkt_len;
            if (copy_len > max_len - (uint16_t)received)
                copy_len = max_len - (uint16_t)received;

            memcpy(buf + received, rxq->buffers[buf_idx], copy_len);
            received += copy_len;

            priv->rx_packets++;
            priv->rx_bytes += pkt_len;
        } else if (pkt_len > 0 && !comp->eop) {
            /* Multi-fragment packet (we only handle single-buf mode) —
             * still accept the data. */
            uint16_t copy_len = pkt_len;
            if (copy_len > max_len - (uint16_t)received)
                copy_len = max_len - (uint16_t)received;

            memcpy(buf + received, rxq->buffers[buf_idx], copy_len);
            received += copy_len;
            priv->rx_bytes += pkt_len;
        }

        /* Refill the RX descriptor that was just consumed */
        {
            struct vmxnet3_rx_desc *rdesc = &rxq->descs[buf_idx];
            memset(rdesc, 0, sizeof(*rdesc));
            rdesc->addr  = rxq->buf_phys[buf_idx];
            rdesc->len   = VMXNET3_RX_BUF_SIZE;
            rdesc->btype = 0;
            rdesc->gen   = (unsigned int)(rxq->gen ? 1 : 0);
        }

        /* Advance in the completion ring */
        rxq->comp_next = (comp_slot + 1) % rxq->size;
        if (rxq->comp_next == 0)
            rxq->comp_gen ^= 1;   /* toggle expected generation */

        /* Re-read the next completion entry */
        comp = &rxq->comp[rxq->comp_next];

        /* Limit processing to one full ring worth of entries */
        if (rxq->comp_next == rxq->next)
            break;
    }

    /* Keep the RX descriptor ring's fill index in sync.
     * Since we refilled the consumed descriptor above, advance
     * the fill index to match the new producer position. */
    rxq->next = rxq->comp_next;

    if (received > 0)
        return received;

    return 0;
}

/* ── Netdevice callbacks ─────────────────────────────────────────── */

static int vmxnet3_netdev_transmit(struct net_device *dev,
                                   const uint8_t *data, uint16_t len)
{
    struct vmxnet3_priv *priv;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct vmxnet3_priv *)dev->priv;
    return vmxnet3_transmit(priv, data, len);
}

static int vmxnet3_netdev_receive(struct net_device *dev,
                                  uint8_t *buf, uint16_t max_len)
{
    struct vmxnet3_priv *priv;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct vmxnet3_priv *)dev->priv;
    return vmxnet3_receive(priv, buf, max_len);
}

/* ── PCI probe ───────────────────────────────────────────────────── */

static int vmxnet3_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(VMXNET3_VENDOR, VMXNET3_DEVICE, pci_dev);
    if (ret < 0)
        return -ENODEV;

    return 0;
}

static int vmxnet3_probe(struct vmxnet3_priv *priv)
{
    struct pci_device pci_dev;
    uint64_t mmio_phys;
    uint32_t vr0, vr1;
    int ret;

    /* Find the vmxnet3 on the PCI bus */
    ret = vmxnet3_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  vmxnet3: device not found\n");
        return ret;
    }

    /* BAR0 contains the MMIO base address (the lower bits encode
     * the BAR type — mask them off to get the physical address). */
    mmio_phys = (uint64_t)(pci_dev.bar[0] & ~0xFULL);
    if (mmio_phys == 0) {
        kprintf("  vmxnet3: invalid MMIO base from BAR0\n");
        return -ENODEV;
    }

    priv->mmio_phys  = mmio_phys;
    priv->mmio_base  = (uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)mmio_phys);
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;

    /* Enable PCI bus mastering so the device can DMA to/from host memory */
    pci_enable_bus_master(&pci_dev);

    /* Read version registers to confirm the device is alive */
    vr0 = vmxnet3_readl(priv, VMXNET3_REG_VR0);
    vr1 = vmxnet3_readl(priv, VMXNET3_REG_VR1);

    kprintf("  vmxnet3: found at %02x:%02x.%d, "
            "MMIO phys 0x%llx (virt 0x%lx), IRQ %d, "
            "vr=0x%08x/0x%08x\n",
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            (unsigned long long)priv->mmio_phys,
            (unsigned long)priv->mmio_base,
            priv->irq_line, vr0, vr1);

    /* Check magic version values */
    if (vr0 != VMXNET3_MAGIC_VR0) {
        kprintf("  vmxnet3: unexpected VR0 magic 0x%08x "
                "(expected 0x%08x)\n", vr0, VMXNET3_MAGIC_VR0);
        /* Continue anyway — older/future revisions may differ */
    }

    /* ── Set up descriptor rings ─────────────────────────────────── */
    ret = vmxnet3_setup_rings(priv);
    if (ret < 0) {
        kprintf("  vmxnet3: ring setup failed (%d)\n", ret);
        return ret;
    }

    /* ── Read MAC address ─────────────────────────────────────────── */
    vmxnet3_get_mac(priv);

    /* ── Activate the device ───────────────────────────────────────── */
    ret = vmxnet3_activate(priv);
    if (ret < 0) {
        kprintf("  vmxnet3: device activation failed (%d)\n", ret);
        vmxnet3_teardown_rings(priv);
        return ret;
    }

    priv->present = 1;
    return 0;
}

/* ── Module / device init entry point ────────────────────────────── */

int vmxnet3_init(void)
{
    int ret;

    memset(&vmxnet3_state, 0, sizeof(vmxnet3_state));
    vmxnet3_state.ifindex = -1;

    ret = vmxnet3_probe(&vmxnet3_state);
    if (ret < 0)
        return ret;

    /* ── Register with netdevice layer ───────────────────────────── */
    {
        struct net_device *ndev = &vmxnet3_state.ndev;

        if (netif_name_to_index("eth1") < 0) {
            memset(ndev, 0, sizeof(*ndev));
            snprintf(ndev->name, sizeof(ndev->name), "eth1");
            memcpy(ndev->mac, vmxnet3_state.mac, 6);
            ndev->transmit = vmxnet3_netdev_transmit;
            ndev->receive  = vmxnet3_netdev_receive;
            ndev->mtu      = 1500;
            ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev->priv     = &vmxnet3_state;

            int ifindex = netif_register(ndev);
            if (ifindex >= 0) {
                vmxnet3_state.ifindex = ifindex;
                kprintf("  vmxnet3: registered as netdevice ifindex=%d\n",
                        ifindex);
            } else {
                kprintf("  vmxnet3: netdevice registration failed\n");
                vmxnet3_deactivate(&vmxnet3_state);
                vmxnet3_teardown_rings(&vmxnet3_state);
                return -EIO;
            }
        }
    }

    kprintf("vmxnet3: driver loaded (TX/RX descriptor rings)\n");
    return 0;
}

void vmxnet3_exit(void)
{
    if (vmxnet3_state.present) {
        /* Unregister from netdevice layer */
        if (vmxnet3_state.ifindex >= 0) {
            netif_unregister(vmxnet3_state.ifindex);
            vmxnet3_state.ifindex = -1;
        }

        /* Deactivate the device */
        vmxnet3_deactivate(&vmxnet3_state);

        /* Free shared memory */
        vmxnet3_teardown_rings(&vmxnet3_state);

        vmxnet3_state.present = 0;
    }
    kprintf("vmxnet3: driver unloaded\n");
}

#ifdef MODULE
#endif

module_init(vmxnet3_init);
module_exit(vmxnet3_exit);

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("VMware vmxnet3 paravirtualized NIC driver (TX/RX descriptor rings)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v000015ADd000007B0*");
#endif
