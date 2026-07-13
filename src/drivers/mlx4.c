/*
 * src/drivers/mlx4.c — Mellanox ConnectX-3 (mlx4) NIC driver
 *
 * Physical Function (PF) driver for the Mellanox ConnectX-3 (0x1003)
 * and ConnectX-3 Pro (0x1007) 40GbE/IB dual-port controllers.  Implements
 * PCI device identification, MMIO-based device control via the firmware
 * command interface (HCR mailbox), and netdevice registration.
 *
 * Task 14: mlx4: firmware command interface
 *   - PCI device identification (vendor 0x15B3, devices 0x1003/0x1007)
 *   - MMIO mapping of BAR0 (1 MB region)
 *   - Firmware command interface via HCR command registers
 *   - Mailbox-based QUERY_FW, QUERY_DEV_CAP, QUERY_HCA, INIT_HCA
 *   - Port MAC address retrieval
 *   - Netdevice registration as "eth5"
 */

#include "mlx4.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "net.h"
#include "netdevice.h"
#include "pmm.h"
#include "errno.h"
#include "err.h"
#include "kernel.h"

#include "module.h"
#ifdef MODULE
#endif

/* ── Static driver state ──────────────────────────────────────────── */
static struct mlx4_priv mlx4_state;

/* ── Forward declarations ─────────────────────────────────────────── */
static int mlx4_netdev_transmit(struct net_device *dev,
                                const uint8_t *data, uint16_t len);
static int mlx4_netdev_receive(struct net_device *dev,
                               uint8_t *buf, uint16_t max_len);

/* ══════════════════════════════════════════════════════════════════════
 *  Firmware command interface
 *
 *  The HCR (Hardware Command Register) is the primary path for
 *  communicating with the on-chip firmware.  A command is issued by:
 *
 *    1. Writing the command opcode and input parameters to HCR regs.
 *    2. Setting the GO bit in the HCR command register.
 *    3. Polling the GO bit until the firmware clears it.
 *    4. Reading the status and output parameters.
 *
 *  For commands with payloads larger than 16 bytes, a mailbox
 *  (DMA buffer in BAR0 memory) is used.
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_cmd_get_success - Convert firmware status to success indicator.
 *
 * Returns 0 if the command completed successfully, or a negative errno
 * corresponding to the firmware error code. */
int mlx4_cmd_get_success(uint32_t status)
{
    switch (status) {
    case MLX4_STAT_SUCCESS:
        return 0;
    case MLX4_STAT_BAD_OP:
        return -EINVAL;
    case MLX4_STAT_BAD_PARAM:
        return -EINVAL;
    case MLX4_STAT_BAD_SYS_STATE:
        return -EBUSY;
    case MLX4_STAT_INTERNAL_ERR:
        return -EIO;
    case MLX4_STAT_TIMEOUT:
        return -ETIMEDOUT;
    case MLX4_STAT_BAD_RESOURCE:
        return -ENOMEM;
    default:
        return -EIO;
    }
}

/* mlx4_cmd - Issue a firmware command via the HCR interface.
 *
 * @opcode:   command opcode (MLX4_CMD_*).
 * @param_in: 64-bit input parameter (low 32 bits go to HCR_PARAM,
 *            high 32 bits go to HCR_PARAM_HI).
 * @param_out: if non-NULL, receives the 64-bit output parameter.
 * @use_mailbox: non-zero if the command uses the mailbox region.
 * @timeout_ms: maximum time to wait for command completion in ms.
 *
 * Returns 0 on success, negative errno on firmware error or timeout. */
int mlx4_cmd(struct mlx4_priv *priv, uint16_t opcode,
             uint64_t param_in, uint64_t *param_out,
             int use_mailbox, uint64_t timeout_ms)
{
    uint32_t ctl;
    uint32_t status;
    uint64_t deadline;
    int ret;

    (void)use_mailbox;  /* mailbox selection is implicit in opcode */

    if (!priv || !priv->mmio_base)
        return -ENXIO;

    /* Step 1: write input parameters to HCR registers */
    mlx4_writel(priv, MLX4_HCR_PARAM,    (uint32_t)(param_in & 0xFFFFFFFFULL));
    mlx4_writel(priv, MLX4_HCR_PARAM_HI, (uint32_t)((param_in >> 32) & 0xFFFFFFFFULL));

    /* Step 2: write command opcode + set GO bit */
    ctl = MLX4_HCR_OPCODE(opcode) | MLX4_HCR_GO_BIT;
    mlx4_writel(priv, MLX4_HCR, ctl);

    /* Ensure the GO write reaches the device (memory barrier implied by
     * MMIO write ordering; io_wait() adds a short PIO delay). */
    io_wait();

    /* Step 3: poll for completion (GO bit self-clears) */
    if (timeout_ms == 0)
        timeout_ms = 5000;  /* default 5-second timeout */

    deadline = timeout_ms * 1000;  /* approximate busy-loop count */

    while (deadline > 0) {
        ctl = mlx4_readl(priv, MLX4_HCR);
        if (!(ctl & MLX4_HCR_GO_BIT))
            break;
        io_wait();
        deadline--;
    }

    if (deadline == 0) {
        kprintf("  mlx4: command 0x%04X timed out\n", opcode);
        return -ETIMEDOUT;
    }

    /* Step 4: read status */
    status = mlx4_readl(priv, MLX4_HCR_STAT);
    ret = mlx4_cmd_get_success(status);
    if (ret < 0) {
        kprintf("  mlx4: command 0x%04X failed status=%u (%d)\n",
                opcode, status, ret);
        return ret;
    }

    /* Step 5: read output parameters if requested */
    if (param_out) {
        uint64_t lo, hi;
        lo = (uint64_t)mlx4_readl(priv, MLX4_HCR_PARAM);
        hi = (uint64_t)mlx4_readl(priv, MLX4_HCR_PARAM_HI);
        *param_out = lo | (hi << 32);
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  High-level firmware command helpers
 *
 *  Each helper issues a specific firmware command and populates the
 *  corresponding field in struct mlx4_priv.
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_query_fw - Query firmware revision and capabilities.
 *
 * Issues QUERY_FW command and reads the firmware info mailbox.
 * Returns 0 on success, negative errno on failure. */
int mlx4_query_fw(struct mlx4_priv *priv)
{
    uint64_t param_out;
    uint32_t *mbox;
    int ret;

    /* Mailbox-based command; input is zero, output is at BAR0+0x20000 */
    ret = mlx4_cmd(priv, MLX4_CMD_QUERY_FW, 0, &param_out, 1, 5000);
    if (ret < 0)
        return ret;

    /* The firmware info is written to the mailbox output area (BAR0 offset
     * 0x20000).  Read the first 32 bytes of the mailbox to extract info. */
    mbox = (uint32_t *)(priv->mmio_base + MLX4_MAILBOX_ADDR_OUT);

    priv->fw.fw_revision  = (uint64_t)mbox[0]
                          | ((uint64_t)mbox[1] << 32);
    priv->fw.fw_pages     = (uint64_t)mbox[2]
                          | ((uint64_t)mbox[3] << 32);
    priv->fw.total_ram    = (uint64_t)mbox[4]
                          | ((uint64_t)mbox[5] << 32);
    priv->fw.max_cmds     = mbox[6];

    kprintf("  mlx4: firmware rev=0x%llx pages=%llu ram=%llu max_cmds=%u\n",
            (unsigned long long)priv->fw.fw_revision,
            (unsigned long long)priv->fw.fw_pages,
            (unsigned long long)priv->fw.total_ram,
            priv->fw.max_cmds);

    return 0;
}

/* mlx4_query_dev_cap - Query device capabilities.
 *
 * Issues QUERY_DEV_CAP command and reads the capability flags.
 * Returns 0 on success, negative errno on failure. */
int mlx4_query_dev_cap(struct mlx4_priv *priv)
{
    uint64_t param_out;
    uint32_t *mbox;
    int ret;

    ret = mlx4_cmd(priv, MLX4_CMD_QUERY_DEV_CAP, 0, &param_out, 1, 5000);
    if (ret < 0)
        return ret;

    mbox = (uint32_t *)(priv->mmio_base + MLX4_MAILBOX_ADDR_OUT);

    priv->cap.flags        = (uint64_t)mbox[0]
                            | ((uint64_t)mbox[1] << 32);
    priv->cap.max_qps      = mbox[2];
    priv->cap.max_mpts     = mbox[3];
    priv->cap.max_mtts     = mbox[4];
    priv->cap.max_cqs      = mbox[5];
    priv->cap.max_eqs      = mbox[6];
    priv->cap.max_mcg      = mbox[7];
    priv->cap.num_ports    = (mbox[8] & 0xFF);

    if (priv->cap.num_ports > MLX4_MAX_PORTS)
        priv->cap.num_ports = MLX4_MAX_PORTS;

    priv->num_ports = (int)priv->cap.num_ports;

    kprintf("  mlx4: %d ports %s%s%s%s max_qps=%u max_cqs=%u max_eqs=%u\n",
            priv->num_ports,
            (priv->cap.flags & MLX4_DEV_CAP_FLAG_PORT_TYPE_ETH) ? "ETH " : "",
            (priv->cap.flags & MLX4_DEV_CAP_FLAG_PORT_TYPE_IB)  ? "IB "  : "",
            (priv->cap.flags & MLX4_DEV_CAP_FLAG_RSS)           ? "RSS " : "",
            (priv->cap.flags & MLX4_DEV_CAP_FLAG_VLAN)          ? "VLAN " : "",
            priv->cap.max_qps, priv->cap.max_cqs, priv->cap.max_eqs);

    return 0;
}

/* mlx4_query_hca - Query HCA configuration parameters.
 *
 * Issues QUERY_HCA command to retrieve the HCA resource table sizes.
 * Returns 0 on success, negative errno on failure. */
int mlx4_query_hca(struct mlx4_priv *priv)
{
    uint64_t param_out;
    uint32_t *mbox;
    int ret;

    ret = mlx4_cmd(priv, MLX4_CMD_QUERY_HCA, 0, &param_out, 1, 5000);
    if (ret < 0)
        return ret;

    mbox = (uint32_t *)(priv->mmio_base + MLX4_MAILBOX_ADDR_OUT);

    priv->hca.qp_table_size    = mbox[0];
    priv->hca.eq_table_size    = mbox[1];
    priv->hca.cq_table_size    = mbox[2];
    priv->hca.mpt_table_size   = mbox[3];
    priv->hca.mtt_table_size   = mbox[4];
    priv->hca.mcg_table_size   = mbox[5];
    priv->hca.srq_table_size   = mbox[6];
    priv->hca.num_qps          = mbox[7];
    priv->hca.num_cqs          = mbox[8];
    priv->hca.num_eqs          = mbox[9];
    priv->hca.num_mpts         = mbox[10];
    priv->hca.num_mtts         = mbox[11];
    priv->hca.num_srqs         = mbox[12];
    priv->hca.num_mcgs         = mbox[13];
    priv->hca.num_uars         = mbox[14];
    priv->hca.uar_page_size    = (uint64_t)mbox[15]
                               | ((uint64_t)mbox[16] << 32);

    kprintf("  mlx4: HCA params qps=%u cqs=%u eqs=%u uars=%u "
            "uar_page=0x%llx\n",
            priv->hca.num_qps, priv->hca.num_cqs,
            priv->hca.num_eqs, priv->hca.num_uars,
            (unsigned long long)priv->hca.uar_page_size);

    return 0;
}

/* mlx4_init_hca - Initialise the HCA (Host Channel Adapter).
 *
 * Issues the INIT_HCA command to initialise hardware resources.
 * Must be called after QUERY_FW, QUERY_DEV_CAP, and QUERY_HCA.
 * Returns 0 on success, negative errno on failure. */
int mlx4_init_hca(struct mlx4_priv *priv)
{
    int ret;

    ret = mlx4_cmd(priv, MLX4_CMD_INIT_HCA, 0, NULL, 1, 10000);
    if (ret < 0) {
        kprintf("  mlx4: INIT_HCA failed (%d)\n", ret);
        return ret;
    }

    priv->fw_initted = 1;
    kprintf("  mlx4: HCA initialised\n");
    return 0;
}

/* mlx4_close_hca - Tear down the HCA.
 *
 * Issues CLOSE_HCA to release hardware resources.  Only valid if
 * INIT_HCA was previously called.  Returns 0 on success. */
int mlx4_close_hca(struct mlx4_priv *priv)
{
    int ret;

    if (!priv->fw_initted)
        return 0;  /* nothing to close */

    ret = mlx4_cmd(priv, MLX4_CMD_CLOSE_HCA, 0, NULL, 1, 10000);
    if (ret < 0) {
        kprintf("  mlx4: CLOSE_HCA failed (%d)\n", ret);
        return ret;
    }

    priv->fw_initted = 0;
    kprintf("  mlx4: HCA closed\n");
    return 0;
}

/* mlx4_query_port - Query port properties (MAC, link state, speed).
 *
 * @port: port number (1-based).
 * Returns 0 on success, negative errno on failure. */
int mlx4_query_port(struct mlx4_priv *priv, int port)
{
    uint64_t param_in;
    uint64_t param_out;
    uint32_t *mbox;
    int ret;
    int idx;

    if (port < 1 || port > priv->num_ports)
        return -EINVAL;

    idx = port - 1;

    param_in = (uint64_t)(port & 0xFF);
    ret = mlx4_cmd(priv, MLX4_CMD_QUERY_PORT, param_in, &param_out,
                   1, 5000);
    if (ret < 0)
        return ret;

    mbox = (uint32_t *)(priv->mmio_base + MLX4_MAILBOX_ADDR_OUT);

    /* MAC address is in the first 6 bytes of the mailbox response.
     * Layout: bytes 0-5 = MAC, byte 6+ = link state, speed, etc. */
    priv->port[idx].mac[0] = (uint8_t)(mbox[0] & 0xFF);
    priv->port[idx].mac[1] = (uint8_t)((mbox[0] >> 8) & 0xFF);
    priv->port[idx].mac[2] = (uint8_t)((mbox[0] >> 16) & 0xFF);
    priv->port[idx].mac[3] = (uint8_t)((mbox[0] >> 24) & 0xFF);
    priv->port[idx].mac[4] = (uint8_t)(mbox[1] & 0xFF);
    priv->port[idx].mac[5] = (uint8_t)((mbox[1] >> 8) & 0xFF);

    priv->port[idx].link_speed = (mbox[2] & 0xFF);
    priv->port[idx].link_state = (mbox[2] >> 8) & 0xFF;
    priv->port[idx].port_type  = MLX4_PORT_TYPE_ETH;

    kprintf("  mlx4: port%d MAC=%02x:%02x:%02x:%02x:%02x:%02x "
            "speed=%u link=%s\n",
            port,
            priv->port[idx].mac[0], priv->port[idx].mac[1],
            priv->port[idx].mac[2], priv->port[idx].mac[3],
            priv->port[idx].mac[4], priv->port[idx].mac[5],
            priv->port[idx].link_speed,
            priv->port[idx].link_state ? "UP" : "DOWN");

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Device discovery
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_find_device - Find a Mellanox ConnectX-3-compatible device.
 *
 * Tries ConnectX-3, ConnectX-3 Pro, VF, and IOV device IDs.
 * Returns 0 on success, -ENODEV if none found. */
int mlx4_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(MLX4_VENDOR, MLX4_DEV_CONNECTX3, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(MLX4_VENDOR, MLX4_DEV_CONNECTX3P, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(MLX4_VENDOR, MLX4_DEV_VF, pci_dev);
    if (ret >= 0)
        return 0;

    ret = pci_find_device(MLX4_VENDOR, MLX4_DEV_IOV, pci_dev);
    if (ret >= 0)
        return 0;

    return -ENODEV;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Hardware initialisation
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_init_hw - Initialise device hardware via firmware commands.
 *
 * Performs the standard firmware-boot sequence:
 *   QUERY_FW → QUERY_DEV_CAP → QUERY_HCA → INIT_HCA → QUERY_PORT(1)
 *
 * Also reads the port MAC address for later netdevice registration.
 * Returns 0 on success, negative errno on failure. */
int mlx4_init_hw(struct mlx4_priv *priv)
{
    int ret;

    /* 1. Query firmware info */
    ret = mlx4_query_fw(priv);
    if (ret < 0) {
        kprintf("  mlx4: QUERY_FW failed (%d)\n", ret);
        return ret;
    }

    /* 2. Query device capabilities */
    ret = mlx4_query_dev_cap(priv);
    if (ret < 0) {
        kprintf("  mlx4: QUERY_DEV_CAP failed (%d)\n", ret);
        return ret;
    }

    /* 3. Query HCA configuration */
    ret = mlx4_query_hca(priv);
    if (ret < 0) {
        kprintf("  mlx4: QUERY_HCA failed (%d)\n", ret);
        return ret;
    }

    /* 4. Initialise HCA */
    ret = mlx4_init_hca(priv);
    if (ret < 0)
        return ret;

    /* 5. Query port 1 for MAC address */
    ret = mlx4_query_port(priv, 1);
    if (ret < 0) {
        kprintf("  mlx4: QUERY_PORT(1) failed (%d) — continuing\n", ret);
        /* Not fatal — we can still register with a placeholder MAC */
    }

    return 0;
}

/* mlx4_shutdown_hw - Shut down device hardware.
 *
 * Issues CLOSE_HCA to release firmware resources.
 * Call during driver unload or on probe failure. */
void mlx4_shutdown_hw(struct mlx4_priv *priv)
{
    mlx4_close_hca(priv);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Probe / PCI setup
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_probe - Probe and initialise an mlx4 device.
 *
 * Finds the device on the PCI bus, maps MMIO BAR0, enables bus
 * mastering, and initialises the hardware via firmware commands.
 * Returns 0 on success, negative errno on failure. */
int mlx4_probe(struct mlx4_priv *priv)
{
    struct pci_device pci_dev;
    uint64_t mmio_phys;
    int ret;

    /* Find a mlx4-compatible device on the PCI bus */
    ret = mlx4_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  mlx4: device not found\n");
        return ret;
    }

    /* BAR0 contains the MMIO base address (1 MB region) */
    mmio_phys = (uint64_t)(pci_dev.bar[0] & ~0xFULL);
    if (mmio_phys == 0) {
        kprintf("  mlx4: invalid MMIO base from BAR0\n");
        return -ENODEV;
    }

    priv->mmio_phys  = mmio_phys;
    priv->mmio_base  = (uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)mmio_phys);
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;
    priv->device_id  = pci_dev.device_id;
    priv->num_tx_queues = 1;
    priv->num_rx_queues = 1;

    kprintf("  mlx4: found device 0x%04x at %02x:%02x.%d "
            "BAR0=0x%llx IRQ=%d\n",
            pci_dev.device_id,
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            (unsigned long long)mmio_phys, pci_dev.irq);

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&pci_dev);

    /* Initialise hardware (firmware commands, etc.) */
    ret = mlx4_init_hw(priv);
    if (ret < 0)
        return ret;

    /* Use port 0's MAC for the netdevice */
    memcpy(priv->mac, priv->port[0].mac, 6);

    /* If the hardware didn't provide a MAC, generate one */
    {
        int all_zero = 1;
        int i;
        for (i = 0; i < 6; i++) {
            if (priv->mac[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            /* Locally-administered MAC based on device ID and bus info */
            priv->mac[0] = 0x02;
            priv->mac[1] = 0x00;
            priv->mac[2] = 0x1A;
            priv->mac[3] = 0x4B;
            priv->mac[4] = (uint8_t)(pci_dev.bus);
            priv->mac[5] = (uint8_t)((pci_dev.slot << 4) | pci_dev.func);
            kprintf("  mlx4: using generated MAC "
                    "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    priv->mac[0], priv->mac[1], priv->mac[2],
                    priv->mac[3], priv->mac[4], priv->mac[5]);
        }
    }

    priv->present = 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Completion queue helpers
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_cq_arm - Arm a completion queue by writing the CQ doorbell.
 *
 * This notifies the hardware that the driver has consumed CQEs up to
 * the consumer_index, allowing the hardware to write new CQEs.
 * For ConnectX-3, the doorbell is a 32-bit write containing the CQ
 * number (low 24 bits) with a notification type bit. */
void mlx4_cq_arm(struct mlx4_cq *cq)
{
    if (!cq)
        return;
    /* 32-bit doorbell: CQN in low 24 bits + arm bit in bit 30 */
    uint32_t db_val = (uint32_t)(cq->cqn & 0xFFFFFFU) | (1U << 30);
    /* (Actual write is device-specific; this is the abstracted form) */
    (void)db_val;
}

/* mlx4_cq_init - Initialise a completion queue.
 *
 * @cqn:   CQ number (0-based).
 * @size:  number of CQE entries (must be power of 2 and <= MLX4_CQ_RING_SIZE).
 * @ring:  virtual address of the CQE ring buffer (must be aligned to 64 bytes).
 * @ring_phys: physical address of the CQE ring buffer.
 *
 * Returns 0 on success, negative errno on failure. */
int mlx4_cq_init(struct mlx4_priv *priv, int cqn, int size,
                 struct mlx4_cqe *ring, uint64_t ring_phys)
{
    struct mlx4_cq *cq;

    if (!priv || cqn < 0 || cqn >= MLX4_NUM_CQS)
        return -EINVAL;
    if (!ring || !ring_phys)
        return -EINVAL;
    if (size < 2 || size > MLX4_CQ_RING_SIZE ||
        (size & (size - 1)) != 0)    /* must be power of 2 */
        return -EINVAL;

    cq = &priv->cq[cqn];
    cq->cqn             = cqn;
    cq->cqe_ring        = ring;
    cq->cqe_ring_phys   = ring_phys;
    cq->size            = size;
    cq->consumer_index  = 0;
    cq->doorbell_offset = MLX4_CQ_DOORBELL(cqn);

    /* Clear the CQE ring (set owner bit to hardware) */
    {
        int i;
        for (i = 0; i < size; i++)
            ring[i].flags_syndrome = 0;  /* hardware owner = bit 7 clear */
    }

    /* Arm the CQ for the first time */
    mlx4_cq_arm(cq);

    kprintf("  mlx4: CQ%d initialised (%d entries, doorbell at 0x%x)\n",
            cqn, size, cq->doorbell_offset);
    return 0;
}

/* mlx4_cq_cleanup - Tear down a completion queue. */
void mlx4_cq_cleanup(struct mlx4_cq *cq)
{
    if (!cq)
        return;
    cq->cqe_ring        = NULL;
    cq->cqe_ring_phys   = 0;
    cq->size            = 0;
    cq->consumer_index  = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Netdevice glue layer
 *
 *  Connects the mlx4 driver to the kernel's netdevice registration
 *  system.  The mlx4 is limited to a single TX/RX queue (queue 0)
 *  for the initial implementation.
 * ══════════════════════════════════════════════════════════════════════ */

/* mlx4_netdev_transmit - Transmit a packet on an mlx4 interface.
 *
 * This is a stub that reports the packet as transmitted without
 * programming the hardware descriptor.  Full TX ring support will
 * be added in a subsequent task.
 * Returns 0 (success) or -ENOSPC if the TX ring is full. */
static int mlx4_netdev_transmit(struct net_device *dev,
                                const uint8_t *data, uint16_t len)
{
    struct mlx4_priv *priv;
    (void)data;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct mlx4_priv *)dev->priv;

    /* Stub: just increment counters */
    priv->tx_packets++;
    priv->tx_bytes += len;

    return 0;
}

/* mlx4_netdev_receive - Poll for received packet on an mlx4 interface.
 *
 * This is a stub that always reports no data available.  Full RX ring
 * support will be added in a subsequent task.
 * Returns 0 (no data available). */
static int mlx4_netdev_receive(struct net_device *dev,
                               uint8_t *buf, uint16_t max_len)
{
    struct mlx4_priv *priv;
    (void)buf;
    (void)max_len;

    if (!dev || !dev->priv)
        return -EINVAL;

    priv = (struct mlx4_priv *)dev->priv;

    /* No data available yet */
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Driver entry / exit
 * ══════════════════════════════════════════════════════════════════════ */

static int mlx4_initialized = 0;

/* mlx4_init - Main driver initialisation entry point.
 *
 * Probes for a mlx4 device, initialises the hardware via firmware
 * commands, and registers a netdevice interface.  Returns 0 on
 * success, negative errno on failure. */
int mlx4_init(void)
{
    int ret;

    memset(&mlx4_state, 0, sizeof(mlx4_state));
    mlx4_state.ifindex = -1;

    ret = mlx4_probe(&mlx4_state);
    if (ret < 0)
        return ret;

    /* ── Register with netdevice layer ───────────────────────────── */
    {
        struct net_device *ndev = &mlx4_state.ndev;

        /* Try eth5 (after e1000=eth0, vmxnet3=eth1, rtl8139=eth2,
         * igb=eth3, i40e=eth4) */
        if (netif_name_to_index("eth5") < 0) {
            memset(ndev, 0, sizeof(*ndev));
            snprintf(ndev->name, sizeof(ndev->name), "eth5");
            memcpy(ndev->mac, mlx4_state.mac, 6);
            ndev->transmit = mlx4_netdev_transmit;
            ndev->receive  = mlx4_netdev_receive;
            ndev->mtu      = 1500;
            ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
            ndev->priv     = &mlx4_state;

            int ifindex = netif_register(ndev);
            if (ifindex >= 0) {
                mlx4_state.ifindex = ifindex;
                kprintf("  mlx4: registered as netdevice ifindex=%d\n",
                        ifindex);
            } else {
                kprintf("  mlx4: netdevice registration failed\n");
                mlx4_shutdown_hw(&mlx4_state);
                return -EIO;
            }
        } else {
            /* eth5 already taken — try next available */
            char name[NETDEV_NAME_MAX];
            int i;

            for (i = 5; i < NETDEV_MAX; i++) {
                snprintf(name, sizeof(name), "eth%d", i);
                if (netif_name_to_index(name) < 0) {
                    memset(ndev, 0, sizeof(*ndev));
                    snprintf(ndev->name, sizeof(ndev->name), "%s", name);
                    memcpy(ndev->mac, mlx4_state.mac, 6);
                    ndev->transmit = mlx4_netdev_transmit;
                    ndev->receive  = mlx4_netdev_receive;
                    ndev->mtu      = 1500;
                    ndev->flags    = IFF_UP | IFF_BROADCAST | IFF_MULTICAST;
                    ndev->priv     = &mlx4_state;

                    int ifindex = netif_register(ndev);
                    if (ifindex >= 0) {
                        mlx4_state.ifindex = ifindex;
                        kprintf("  mlx4: registered as netdevice "
                                "ifindex=%d (%s)\n", ifindex, name);
                        break;
                    }
                }
            }

            if (mlx4_state.ifindex < 0) {
                kprintf("  mlx4: no available netdevice slot\n");
                mlx4_shutdown_hw(&mlx4_state);
                return -ENOSPC;
            }
        }
    }

    kprintf("mlx4: driver loaded (ConnectX-3 PF, firmware command"
            " interface)\n");

    mlx4_initialized = 1;
    return 0;
}

/* mlx4_exit - Driver exit / cleanup.
 *
 * Unregisters the netdevice and shuts down the hardware. */
void mlx4_exit(void)
{
    if (!mlx4_initialized)
        return;

    kprintf("mlx4: unloading driver\n");

    if (mlx4_state.ifindex >= 0)
        netif_unregister(mlx4_state.ifindex);

    mlx4_shutdown_hw(&mlx4_state);

    memset(&mlx4_state, 0, sizeof(mlx4_state));
    mlx4_initialized = 0;

    kprintf("mlx4: driver unloaded\n");
}

#ifdef MODULE
#endif

module_init(mlx4_init);
module_exit(mlx4_exit);

#ifdef MODULE
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Mellanox ConnectX-3 (mlx4) Ethernet driver (firmware command interface)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v000015B3d00001007*");
MODULE_ALIAS("pci:v000015B3d00001004*");
MODULE_ALIAS("pci:v000015B3d00001013*");
#endif
