/*
 * nvme.c — NVMe (NVM Express) PCI driver with multi-queue support
 *          and multipath (same NSID across multiple controllers)
 *
 * Features:
 *   - Per-CPU I/O submission/completion queue pairs (qid = CPU index + 1)
 *   - Block device registration via blockdev layer
 *   - Proper doorbell stride handling
 *   - NVMe multipath: round-robin across paths, per-path statistics
 *   - ANA-less multipath (simple: same NSID -> multipath device)
 *
 * Architecture:
 *   Admin queue (qid 0): controller configuration, queue creation
 *   I/O queues (qid 1..N): per-CPU I/O submission and completion
 *   Each CPU submits I/O to its own queue for lockless fast path
 */

#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "smp.h"
#include "blockdev.h"
#include "heap.h"      /* kmalloc, kfree */
#include "idt.h"
#include "apic.h"
#include "errno.h"
#ifdef MODULE
#include "module.h"
#endif

static struct nvme_ctrl g_nvme_ctrl;
static int g_nvme_init_done = 0;

/* Saved PCI device for interrupt setup (MSI-X / MSI / INTx) */
static struct pci_device g_nvme_pci_dev;
static struct pci_interrupt_config g_nvme_int_cfg;
static int g_nvme_pci_saved = 0;

/* ── Multipath support ──────────────────────────────────────────────── */

/* Maximum number of paths per namespace */
#define NVME_MPATH_MAX_PATHS   4

/* Per-path state */
struct nvme_path {
    int           active;
    int           ctrl_index;    /* controller index (0 = g_nvme_ctrl) */
    uint32_t      nsid;          /* namespace ID on this controller */
    int           dev_id;        /* block device ID of this path */

    /* Round-robin state */
    int           last_used;     /* tick-based last use time */

    /* Statistics */
    struct nvme_path_stats stats;
};

/* Multipath device */
struct nvme_mpath_dev {
    int             active;
    int             mp_dev_id;   /* multipath block device ID */
    uint32_t        nsid;        /* shared NSID */
    uint64_t        sector_count;
    uint32_t        sector_size;

    /* Paths */
    int             nr_paths;
    struct nvme_path paths[NVME_MPATH_MAX_PATHS];

    /* Round-robin next index */
    int             rr_next;
};

/* Multipath devices (one per unique NSID seen across controllers) */
#define NVME_MPATH_MAX_DEVS  4
static struct nvme_mpath_dev g_mpath_devs[NVME_MPATH_MAX_DEVS];
static int g_mpath_count = 0;
static spinlock_t g_mpath_lock;

/* Forward declarations for multipath functions */
static int nvme_mpath_submit(struct blk_request *req);
static struct nvme_mpath_dev *nvme_mpath_find_or_create(uint32_t nsid,
                                                         int ctrl_index,
                                                         int path_dev_id,
                                                         uint64_t sector_count,
                                                         uint32_t sector_size);

/* ── MMIO accessors ───────────────────────────────────────────────── */

static inline uint32_t nvme_read32(struct nvme_ctrl *ctrl, uint64_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(ctrl->regs + reg);
}

static inline void nvme_write32(struct nvme_ctrl *ctrl, uint64_t reg, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(ctrl->regs + reg) = val;
}

static inline uint64_t nvme_read64(struct nvme_ctrl *ctrl, uint64_t reg) {
    return *(volatile uint64_t *)(uintptr_t)(ctrl->regs + reg);
}

static inline void nvme_write64(struct nvme_ctrl *ctrl, uint64_t reg, uint64_t val) {
    *(volatile uint64_t *)(uintptr_t)(ctrl->regs + reg) = val;
}

/* ── Queue doorbell ring helpers ───────────────────────────────────── */

/** Doorbell offset for a submission queue */
static inline uint64_t nvme_sq_doorbell(struct nvme_ctrl *ctrl, uint16_t qid) {
    return 0x1000 + (uint64_t)(2 * qid) * ctrl->doorbell_stride;
}

/** Doorbell offset for a completion queue */
static inline uint64_t nvme_cq_doorbell(struct nvme_ctrl *ctrl, uint16_t qid) {
    return 0x1000 + (uint64_t)(2 * qid + 1) * ctrl->doorbell_stride;
}

/* ── Wait for controller ready state ───────────────────────────────── */

static int nvme_wait_ready(struct nvme_ctrl *ctrl, int ready, int timeout_ms) {
    uint32_t csts;
    int timeout = 0;
    while (timeout < timeout_ms) {
        csts = nvme_read32(ctrl, NVME_REG_CSTS);
        if (((csts & NVME_CSTS_RDY) != 0) == ready)
            return 0;
        /* Busy-wait ~1ms */
        for (volatile int i = 0; i < 100000; i++);
        timeout++;
    }
    return -ETIMEDOUT;
}

/* ── PCI probe ─────────────────────────────────────────────────────── */

static int nvme_probe_pci(void) {
    struct pci_device pci;
    int ret = pci_find_class(NVME_PCI_CLASS, NVME_PCI_SUBCLASS, &pci);
    if (ret < 0) {
        /* Try with prog_if filter */
        struct pci_device pci2;
        /* Combine subclass and prog_if into a single filter value */
        uint32_t sub_prog = ((uint32_t)NVME_PCI_PROG_IF << 16) | NVME_PCI_SUBCLASS;
        int ret2 = pci_find_class(NVME_PCI_CLASS, (uint8_t)sub_prog, &pci2);
        if (ret2 < 0)
            return -EINVAL;
        pci = pci2;
    }

    /* Enable bus mastering */
    pci_enable_bus_master(&pci);

    /* Get BAR0 (MMIO registers) */
    uint32_t bar0 = pci.bar[0];
    if (!(bar0 & 1)) {
        /* MMIO BAR */
        uint64_t mmio_base = (bar0 & 0xFFFFFFF0);
        if (mmio_base == 0)
            return -EIO;
        g_nvme_ctrl.phys_regs = mmio_base;
        g_nvme_ctrl.regs = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);
    } else {
        /* I/O BAR not supported for NVMe */
        return -EIO;
    }

    g_nvme_ctrl.irq = pci.irq;

    /* Read capabilities */
    uint64_t cap = nvme_read64(&g_nvme_ctrl, NVME_REG_CAP);
    uint32_t version = nvme_read32(&g_nvme_ctrl, NVME_REG_VS);

    g_nvme_ctrl.max_q_depth = (uint32_t)((cap >> 16) & 0xFFFF) + 1;

    /* Doorbell stride: 2^(dstrd + 2) bytes */
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xF);
    g_nvme_ctrl.doorbell_stride = 4 << dstrd;

    /* MPSMIN: minimum memory page size exponent (shift from 4096 base) */
    g_nvme_ctrl.mpsmin = (uint8_t)((cap >> 48) & 0xF);

    kprintf("[NVME] Found controller: VID=0x%04X DID=0x%04X IRQ=%d\n",
            pci.vendor_id, pci.device_id, pci.irq);
    kprintf("[NVME] Version %d.%d.%d, max queue depth %u, doorbell stride %u\n",
            (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF,
            g_nvme_ctrl.max_q_depth, g_nvme_ctrl.doorbell_stride);

    g_nvme_ctrl.present = 1;

    /* Save PCI device for MSI-X / interrupt setup */
    g_nvme_pci_dev = pci;
    g_nvme_pci_saved = 1;

    return 0;
}

/* ── Admin queue setup ─────────────────────────────────────────────── */

static int nvme_setup_admin_queues(void) {
    /* Allocate one page for admin SQ and one for admin CQ */
    uint64_t sq_frame = pmm_alloc_frame();
    uint64_t cq_frame = pmm_alloc_frame();
    if (unlikely(!sq_frame || !cq_frame)) {
        if (sq_frame) pmm_free_frame(sq_frame);
        return -EIO;
    }

    g_nvme_ctrl.admin_sq = PHYS_TO_VIRT((void*)(uintptr_t)(sq_frame * 4096));
    g_nvme_ctrl.admin_cq = PHYS_TO_VIRT((void*)(uintptr_t)(cq_frame * 4096));
    g_nvme_ctrl.admin_sq_phys = sq_frame * 4096;
    g_nvme_ctrl.admin_cq_phys = cq_frame * 4096;

    memset(g_nvme_ctrl.admin_sq, 0, 4096);
    memset(g_nvme_ctrl.admin_cq, 0, 4096);

    g_nvme_ctrl.admin_sq_tail = 0;
    g_nvme_ctrl.admin_cq_head = 0;

    /* Set admin queue attributes (AQA) */
    /* AQA: bits 0-11: admin SQ size, bits 16-27: admin CQ size */
    uint32_t qsize = 64; /* 64 entries each */
    nvme_write32(&g_nvme_ctrl, NVME_REG_AQA, qsize | (qsize << 16));

    /* Set admin SQ base address (ASQ) and admin CQ base (ACQ) */
    nvme_write64(&g_nvme_ctrl, NVME_REG_ASQ, g_nvme_ctrl.admin_sq_phys);
    nvme_write64(&g_nvme_ctrl, NVME_REG_ACQ, g_nvme_ctrl.admin_cq_phys);

    return 0;
}

/* ── Controller enable/disable ──────────────────────────────────────── */

static int nvme_enable_controller(void) {
    int ret;

    /* Wait for CSTS.RDY = 0 (controller not ready) */
    ret = nvme_wait_ready(&g_nvme_ctrl, 0, 2000);
    if (ret < 0) {
        kprintf("[NVME] Timeout waiting for CSTS.RDY=0\n");
        return -EIO;
    }

    /* Set CC: enable, page size (4096 = 0 shift), MPS=0, CSS=NVM */
    uint32_t cc = NVME_CC_ENABLE | NVME_CC_CSS_NVM |
                  (0 << NVME_CC_MPS_SHIFT) |
                  (NVME_CC_IOSQES << 20) |
                  (NVME_CC_IOCQES << 16);
    nvme_write32(&g_nvme_ctrl, NVME_REG_CC, cc);

    /* Wait for CSTS.RDY = 1 */
    ret = nvme_wait_ready(&g_nvme_ctrl, 1, 2000);
    if (ret < 0) {
        kprintf("[NVME] Timeout waiting for CSTS.RDY=1\n");
        return -EIO;
    }

    return 0;
}

/*
 * ── Sanitize operation (Item 187) ──────────────────────────────────
 *
 * Submit an NVMe sanitize command to cryptographically erase or
 * overwrite the entire NVM subsystem.
 *
 * @action:   NVME_SANITIZE_ACTION_BLOCK_ERASE (1), OVERWRITE (2),
 *            or CRYPTO_ERASE (3).
 * @overwrite_pass_count: number of overwrite passes (1..16 for
 *            Overwrite action; ignored for Block Erase and Crypto Erase).
 *
 * Returns 0 on successful submission, -1 on error.
 *
 * NOTE: The sanitize operation runs asynchronously in the controller.
 * This function waits for command acceptance (completion entry), not
 * for the sanitize to finish.  Use nvme_sanitize_status() to poll for
 * completion via Get Features.
 */
int nvme_sanitize(int action, int overwrite_pass_count) {
    if (!g_nvme_ctrl.present)
        return -EIO;

    if (action < NVME_SANITIZE_ACTION_BLOCK_ERASE ||
        action > NVME_SANITIZE_ACTION_CRYPTO_ERASE)
        return -EIO;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_SANITIZE;          /* opcode */
    /* cdw10: bits [2:0] = sanitize action (SANACT) */
    cmd.cdw10 = (uint32_t)(action & 0x7);
    /* cdw11: overwrite pass count (only meaningful for Overwrite action) */
    if (action == NVME_SANITIZE_ACTION_OVERWRITE) {
        if (overwrite_pass_count < 1) overwrite_pass_count = 1;
        if (overwrite_pass_count > 16) overwrite_pass_count = 16;
        cmd.cdw11 = (uint32_t)(overwrite_pass_count & 0xFF);
    }
    /* No data transfer — no PRP needed */

    kprintf("[NVME] Submitting sanitize command (action=%d, overwrite_passes=%d)...\n",
            action, (action == NVME_SANITIZE_ACTION_OVERWRITE) ? overwrite_pass_count : 0);

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret != 0) {
        kprintf("[NVME] Sanitize command submission FAILED (timeout or error)\n");
        return -EIO;
    }

    /* Check completion status */
    uint16_t status = cqe.status;
    if (status & 0x0001) {
        /* Bit 0 set = error */
        uint8_t sc  = (uint8_t)((status >> 1) & 0xFF);  /* Status Code */
        uint8_t sct = (uint8_t)((status >> 9) & 0x7);   /* Status Code Type */
        kprintf("[NVME] Sanitize command rejected: SCT=%u SC=%u\n",
                (unsigned)sct, (unsigned)sc);
        return -EINVAL;
    }

    kprintf("[NVME] Sanitize operation ACCEPTED by controller — running in background.\n"
            "        Use 'nvme sanitize-status' to check progress.\n");
    return 0;
}

/* ── Sanitize status query (via Get Features) ─────────────────────── */

/**
 * nvme_sanitize_get_status — Query sanitize progress via Get Features (FID 0x81).
 *
 * Sends a Get Features admin command with Feature Identifier 0x81
 * (Sanitize Status) and populates the provided @status structure with
 * the controller's sanitize progress, current status, and estimated
 * time to completion.
 *
 * @status   Pointer to a struct nvme_sanitize_status to receive the data.
 *
 * Returns 0 on success, negative errno on failure.
 */
int nvme_sanitize_get_status(struct nvme_sanitize_status *status)
{
    if (!g_nvme_ctrl.present || !status)
        return -EINVAL;

    /* Allocate a page for the Get Features data buffer */
    uint64_t data_frame = pmm_alloc_frame();
    if (unlikely(!data_frame))
        return -ENOMEM;

    uint64_t data_phys = data_frame * 4096;
    void *data_virt = PHYS_TO_VIRT((void*)(uintptr_t)data_phys);
    memset(data_virt, 0, 4096);

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_GET_FEATURES;
    cmd.nsid = 0;                              /* not namespace-specific */
    cmd.prp1 = data_phys;
    cmd.cdw10 = NVME_FEAT_SANITIZE_STATUS;     /* Feature Identifier 0x81 */
    /* cdw11: SEL=0 (current), no data structure for Sanitize Status select */
    cmd.cdw11 = 0;

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        /* Copy sanitize status data from the returned buffer.
         * The struct nvme_sanitize_status is packed and matches the
         * NVMe-specified layout at offset 0 of the data buffer. */
        memcpy(status, data_virt, sizeof(struct nvme_sanitize_status));
    } else {
        kprintf("[NVME] Get Features (Sanitize Status) command failed: %d\n", ret);
    }

    pmm_free_frame(data_frame);
    return ret;
}

/* ── Admin command submit ──────────────────────────────────────────── */

/* CID counter for admin commands, used to distinguish AER completions */
static uint16_t g_admin_cid_counter = 0;

/* Forward declarations for AER (defined below) */
static int  g_aer_pending = 0;
static void nvme_aer_handle_event(uint32_t cdw0, uint16_t status);
static int  nvme_aer_submit(void);

int nvme_submit_admin_cmd(struct nvme_sq_entry *cmd, struct nvme_cq_entry *cqe)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.admin_sq)
        return -EIO;

    /* Assign a CID (Command Identifier) in bits 31:16 of cdw0.
     * This lets us distinguish our completion from any AER completions
     * that may arrive before the sync command completes. */
    uint16_t cid = g_admin_cid_counter++;
    cmd->cdw0 = (cmd->cdw0 & 0x0000FFFF) | ((uint32_t)cid << 16);

    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)g_nvme_ctrl.admin_sq;
    uint16_t tail = g_nvme_ctrl.admin_sq_tail;

    /* Copy command to submission queue */
    memcpy(&sq[tail], cmd, sizeof(struct nvme_sq_entry));
    tail = (uint16_t)((tail + 1) % 64);
    g_nvme_ctrl.admin_sq_tail = tail;

    __sync_synchronize();

    /* Ring the admin SQ doorbell (qid=0) */
    uint32_t sq_doorbell = (uint32_t)nvme_sq_doorbell(&g_nvme_ctrl, 0);
    nvme_write32(&g_nvme_ctrl, sq_doorbell, tail);

    /* Spin-wait for completion */
    struct nvme_cq_entry *cq = (struct nvme_cq_entry *)g_nvme_ctrl.admin_cq;
    uint16_t head = g_nvme_ctrl.admin_cq_head;
    uint32_t timeout = 1000000;
    while (timeout--) {
        if (cq[head].status != 0xFFFF) {
            /* Drain any AER completions that arrived before our command */
            if (cq[head].cid != cid) {
                /* Consume and re-arm for the unexpected completion */
                cq[head].status = 0xFFFF;
                head = (uint16_t)((head + 1) % 64);
                g_nvme_ctrl.admin_cq_head = head;
                nvme_write32(&g_nvme_ctrl,
                             nvme_cq_doorbell(&g_nvme_ctrl, 0), head);
                continue;
            }

            /* Got our completion */
            if (cqe)
                memcpy(cqe, &cq[head], sizeof(struct nvme_cq_entry));
            /* Mark as consumed */
            cq[head].status = 0xFFFF;
            head = (uint16_t)((head + 1) % 64);
            g_nvme_ctrl.admin_cq_head = head;

            /* Ring the admin CQ doorbell to re-arm */
            nvme_write32(&g_nvme_ctrl,
                         nvme_cq_doorbell(&g_nvme_ctrl, 0), head);

            return 0;
        }
        __asm__ volatile("pause");
    }

    return -EINVAL;
}

/* ── Parse power state descriptors from identify controller data ──── */

/** Parse NPSS and power state descriptors from raw Identify Controller data.
 *  Called from nvme_identify_ctrl() after a successful Identify command.
 *  @id_ctrl_data  Pointer to the 4096-byte Identify Controller data buffer. */
static void nvme_parse_power_states(const void *id_ctrl_data)
{
    const uint8_t *data = (const uint8_t *)id_ctrl_data;
    uint8_t npss;

    npss = data[NVME_ID_CTRL_NPSS_OFFSET];
    if (npss > NVME_MAX_POWER_STATES)
        npss = NVME_MAX_POWER_STATES;

    g_nvme_ctrl.npss = npss;

    for (int i = 0; i < (int)npss; i++) {
        const struct nvme_power_state_desc *src =
            (const struct nvme_power_state_desc *)
                (data + NVME_ID_CTRL_PSDESC_OFFSET + i * (int)sizeof(struct nvme_power_state_desc));
        memcpy(&g_nvme_ctrl.power_states[i], src,
               sizeof(struct nvme_power_state_desc));
    }

    g_nvme_ctrl.power_states_parsed = 1;
}

/* ── Identify controller ──────────────────────────────────────────── */

int nvme_identify_ctrl(struct nvme_identify_ctrl *id) {
    if (!g_nvme_ctrl.present || !id)
        return -EINVAL;

    /* Allocate a page for the identify data (physical contiguous) */
    uint64_t data_frame = pmm_alloc_frame();
    if (unlikely(!data_frame)) return -ENOMEM;

    uint64_t data_phys = data_frame * 4096;
    void *data_virt = PHYS_TO_VIRT((void*)(uintptr_t)data_phys);
    memset(data_virt, 0, 4096);

    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.prp1 = data_phys;
    cmd.cdw10 = NVME_IDENTIFY_CTRL;

    struct nvme_cq_entry cqe;
    int ret = nvme_submit_admin_cmd(&cmd, &cqe);

    if (ret == 0) {
        memcpy(id, data_virt, sizeof(struct nvme_identify_ctrl));
        g_nvme_ctrl.nn = id->nn;
        /* Clamp to our array size — all per-namespace arrays (ns_sector_count,
         * ns_sector_size, ns_blkdev_id) are sized NVME_MAX_NS. Without this
         * cap, a controller claiming >NVME_MAX_NS namespaces causes OOB
         * array access in nvme_blk_submit() and nvme_deallocate(). */
        if (g_nvme_ctrl.nn > NVME_MAX_NS)
            g_nvme_ctrl.nn = NVME_MAX_NS;
        g_nvme_ctrl.sq_entry_size = 1U << (id->sqes & 0x0F);
        g_nvme_ctrl.cq_entry_size = 1U << ((id->cqes >> 4) & 0x0F);

        /* Parse power state descriptors from raw identify data */
        nvme_parse_power_states(data_virt);
    } else {
        kprintf("[NVME] Identify controller command failed\n");
    }

    pmm_free_frame(data_frame);
    return ret;
}

/* ── Identify namespace ────────────────────────────────────────────── */

static int nvme_identify_ns(uint32_t nsid, struct nvme_identify_ns *id) {
    if (!g_nvme_ctrl.present || !id || nsid == 0)
        return -EINVAL;

    uint64_t data_frame = pmm_alloc_frame();
    if (unlikely(!data_frame)) return -ENOMEM;

    uint64_t data_phys = data_frame * 4096;
    void *data_virt = PHYS_TO_VIRT((void*)(uintptr_t)data_phys);
    memset(data_virt, 0, 4096);

    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = data_phys;
    cmd.cdw10 = NVME_IDENTIFY_NS;

    struct nvme_cq_entry cqe;
    int ret = nvme_submit_admin_cmd(&cmd, &cqe);

    if (ret == 0)
        memcpy(id, data_virt, sizeof(struct nvme_identify_ns));

    pmm_free_frame(data_frame);
    return ret;
}

/* ── Set features: Number of Queues ────────────────────────────────── */

static int nvme_set_num_queues(uint32_t nr_queues) {
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;               /* No data transfer for NumberOfQueues */
    cmd.cdw10 = NVME_FEAT_NUMBER_OF_QUEUES;
    /* cdw11: number of I/O submission queues requested (lower 16 bits) */
    /*         and completion queues (upper 16 bits) — 0-based values. */
    cmd.cdw11 = (nr_queues << 16) | nr_queues;

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);

    if (ret == 0) {
        /* The completion entry's cdw0 contains the number of queues granted:
         * lower 16 bits = NCQR (number of I/O completion queues - 1)
         * upper 16 bits = NSQR (number of I/O submission queues - 1)
         */
        uint32_t granted = cqe.cdw0;
        uint32_t granted_cq = (granted & 0xFFFF) + 1;
        uint32_t granted_sq = (granted >> 16) + 1;
        uint32_t actual = (granted_cq < granted_sq) ? granted_cq : granted_sq;
        if (actual < nr_queues) {
            kprintf("[NVME] Requested %u I/O queues, granted %u\n",
                    nr_queues, actual);
        }
        ret = (int)actual;
    }

    return ret;
}

/* ── I/O queue helpers ─────────────────────────────────────────────── */

/* Forward declarations for queue create/delete admin commands */
static int nvme_create_cq(uint16_t cqid, uint64_t addr, uint16_t size, uint16_t iv);
static int nvme_create_sq(uint16_t sqid, uint64_t addr, uint16_t size, uint16_t cqid);
int nvme_delete_cq(uint16_t cqid);
int nvme_delete_sq(uint16_t sqid);

/** Get the I/O queue for the current CPU (falls back to queue 0 if out of range) */
static inline struct nvme_io_queue *nvme_get_io_queue(void) {
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || (uint32_t)cpu >= g_nvme_ctrl.nr_io_queues)
        cpu = 0;
    return &g_nvme_ctrl.io_queues[cpu];
}

/** Create an I/O completion queue */
static int nvme_create_io_cq(struct nvme_io_queue *q) {
    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* Allocate a physically contiguous page for the CQ */
    uint64_t cq_frame = pmm_alloc_frame();
    if (unlikely(!cq_frame)) return -ENOMEM;

    q->cq_virt = PHYS_TO_VIRT((void*)(uintptr_t)(cq_frame * 4096));
    q->cq_phys = cq_frame * 4096;
    memset(q->cq_virt, 0, 4096);
    q->cq_head = 0;

    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = q->cq_phys;
    /* cdw10: [31] PC (physically contiguous)=1, [16] IEN (IRQ enabled)=1, [15:0] CQ ID */
    cmd.cdw10 = (1u << 31) | (1u << 16) | q->qid;
    /* cdw11: [31:16] CQ size (1-based), [15:0] IRQ vector */
    cmd.cdw11 = ((uint32_t)(q->cq_size - 1) << 16) | (uint16_t)q->irq_vector;

    struct nvme_cq_entry cqe;
    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret < 0) {
        kprintf("[NVME] Create I/O CQ %u failed\n", q->qid);
        pmm_free_frame(cq_frame);
        q->cq_virt = NULL;
        q->cq_phys = 0;
        return -EINVAL;
    }

    return 0;
}

/** Create an I/O submission queue (associated with a CQ) */
static int nvme_create_io_sq(struct nvme_io_queue *q) {
    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* Allocate a physically contiguous page for the SQ */
    uint64_t sq_frame = pmm_alloc_frame();
    if (unlikely(!sq_frame)) {
        return -ENOMEM;
    }

    q->sq_virt = PHYS_TO_VIRT((void*)(uintptr_t)(sq_frame * 4096));
    q->sq_phys = sq_frame * 4096;
    memset(q->sq_virt, 0, 4096);
    q->sq_tail = 0;

    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    cmd.prp1 = q->sq_phys;
    /* cdw10: [31] PC=1, [15:0] SQ ID */
    cmd.cdw10 = (1u << 31) | q->qid;
    /* cdw11: [31:16] SQ size (1-based), [15:0] CQ ID */
    cmd.cdw11 = ((uint32_t)(q->sq_size - 1) << 16) | q->qid;

    struct nvme_cq_entry cqe;
    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret < 0) {
        kprintf("[NVME] Create I/O SQ %u failed\n", q->qid);
        pmm_free_frame(sq_frame);
        q->sq_virt = NULL;
        q->sq_phys = 0;
        return -EINVAL;
    }

    return 0;
}

/* ── I/O queue setup ───────────────────────────────────────────────── */

static int nvme_setup_io_queues(void) {
    int nr_cpus = smp_get_cpu_count();
    if (nr_cpus < 1) nr_cpus = 1;
    if (nr_cpus > NVME_IO_QUEUE_MAX) nr_cpus = NVME_IO_QUEUE_MAX;

    /* Negotiate queue count with the controller */
    int nr_queues = nvme_set_num_queues((uint32_t)nr_cpus);
    if (nr_queues < 0) {
        kprintf("[NVME] Failed to negotiate queue count, using 1\n");
        nr_queues = 1;
    }
    if (nr_queues > nr_cpus) nr_queues = nr_cpus;

    kprintf("[NVME] Setting up %d I/O queue pairs\n", nr_queues);

    g_nvme_ctrl.nr_io_queues = (uint32_t)nr_queues;

    for (int i = 0; i < nr_queues; i++) {
        struct nvme_io_queue *q = &g_nvme_ctrl.io_queues[i];
        memset(q, 0, sizeof(*q));
        q->valid    = 0;
        q->cpu_id   = (uint8_t)i;
        q->qid      = (uint16_t)(i + 1);  /* qid 0 is admin queue */
        q->sq_size  = NVME_IO_QUEUE_SIZE;
        q->cq_size  = NVME_IO_QUEUE_SIZE;
        q->stride   = g_nvme_ctrl.doorbell_stride;
        /* IRQ vector: for MSI-X, use entry index i; for MSI/INTx, use 0 */
        if (g_nvme_int_cfg.type == 2)
            q->irq_vector = i;
        else
            q->irq_vector = 0;

        /* Create completion queue first (SQ depends on CQ) */
        if (nvme_create_io_cq(q) < 0) {
            kprintf("[NVME] Failed to create I/O CQ %d\n", q->qid);
            continue;
        }

        /* Then create submission queue associated with this CQ */
        if (nvme_create_io_sq(q) < 0) {
            kprintf("[NVME] Failed to create I/O SQ %d\n", q->qid);
            /* Rollback: delete CQ on the controller, then free host memory */
            nvme_delete_cq(q->qid);
            pmm_free_frame((uint64_t)q->cq_phys / 4096);
            q->cq_virt = NULL;
            q->cq_phys = 0;
            continue;
        }

        q->valid = 1;
    }

    /* Count how many queues were actually created */
    int created = 0;
    for (int i = 0; i < nr_queues; i++) {
        if (g_nvme_ctrl.io_queues[i].valid)
            created++;
    }

    kprintf("[NVME] %d/%d I/O queue pairs active\n", created, nr_queues);
    return created > 0 ? 0 : -1;
}

/* ── I/O submission (called from blockdev submit_fn) ───────────────── */

/** Submit an I/O read/write command on a specific I/O queue */
static int nvme_io_submit_on_queue(struct nvme_io_queue *q, uint32_t nsid,
                                       int is_write, uint64_t lba,
                                       uint32_t count,
                                       uint64_t prp1, uint64_t prp2,
                                       uint32_t nr_pages) {
    (void)nr_pages;
    if (!q || !q->valid || !q->sq_virt)
        return -EIO;

    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)q->sq_virt;
    uint16_t tail = q->sq_tail;

    struct nvme_sq_entry *entry = &sq[tail];

    memset(entry, 0, sizeof(struct nvme_sq_entry));
    entry->cdw0 = (uint32_t)(is_write ? NVME_IO_WRITE : NVME_IO_READ);
    entry->nsid = nsid;
    entry->prp1 = prp1;
    if (prp2) entry->prp2 = prp2;
    entry->cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    entry->cdw11 = (uint32_t)((lba >> 32) & 0xFFFFFFFF);
    /* cdw12: [15:0] number of logical blocks (0-based: 0 = 1 block) */
    entry->cdw12 = count - 1;

    /* Update tail (wrapping circularly) */
    tail = (uint16_t)((tail + 1) % q->sq_size);

    /* Memory barrier: ensure the command is visible before ringing doorbell */
    __sync_synchronize();

    q->sq_tail = tail;
    nvme_write32(&g_nvme_ctrl, nvme_sq_doorbell(&g_nvme_ctrl, q->qid), tail);

    return 0;
}

/** Poll an I/O completion queue for a specific slot */
static int nvme_poll_io_cq(struct nvme_io_queue *q, uint32_t timeout_loops) {
    struct nvme_cq_entry *cq = (struct nvme_cq_entry *)q->cq_virt;
    uint16_t head = q->cq_head;

    while (timeout_loops--) {
        if (cq[head].status != 0xFFFF) {
            /* Check for errors */
            uint16_t status = cq[head].status;
            uint16_t sc = (status >> 1) & 0xFF;  /* Status Code */
            uint16_t sct = (status >> 9) & 0x7;  /* Status Code Type */

            /* Mark as consumed */
            cq[head].status = 0xFFFF;
            head = (uint16_t)((head + 1) % q->cq_size);
            q->cq_head = head;

            /* Re-arm CQ doorbell */
            nvme_write32(&g_nvme_ctrl, nvme_cq_doorbell(&g_nvme_ctrl, q->qid), head);

            if (sct != 0 || sc != 0) {
                return -EIO;
            }
            /* Also check for AER completions on the admin CQ */
            nvme_aer_poll();
            return 0;
        }
        __asm__ volatile("pause");
    }

    return -ETIMEDOUT;  /* timeout */
}

/* ── NVMe deallocate (Dataset Management) ─────────────────────────── */

/**
 * nvme_deallocate — Deallocate a range of LBAs on a namespace (TRIM/DISCARD).
 *
 * Sends a Dataset Management command with the Deallocate bit set via the
 * current CPU's I/O queue.  The NVMe controller will mark the specified
 * LBA range as deallocated; subsequent reads may return zeroes or
 * indeterminate data.
 *
 * @ns_id    Namespace identifier (1-based).
 * @lba      Starting LBA.
 * @count    Number of LBAs to deallocate.
 *
 * Returns 0 on success, -1 on failure.
 */
int nvme_deallocate(int ns_id, uint64_t lba, uint32_t count) {
    if (ns_id < 1 || (uint32_t)ns_id > g_nvme_ctrl.nn || count == 0)
        return -EIO;

    struct nvme_io_queue *q = nvme_get_io_queue();
    if (!q || !q->valid)
        return -EIO;

    /* Allocate one physically-contiguous page for the DSM range list.
     * The NVMe Dataset Management range is a 16-byte structure.
     * We send a single range per command. */
    uint64_t range_frame = pmm_alloc_frame();
    if (unlikely(!range_frame))
        return -ENOMEM;

    uint64_t range_phys = range_frame * 4096;
    uint32_t *range = (uint32_t *)PHYS_TO_VIRT(range_phys);

    /* Build the DSM range entry:
     *   bytes 0-3:  Context attributes (0 = no context)
     *   bytes 4-7:  Length (number of LBAs - 1, 0-based)
     *   bytes 8-15: Starting LBA */
    range[0] = 0;                                      /* attributes */
    range[1] = count - 1;                               /* length (0-based) */
    {
        uint64_t lba_tmp = lba;
        memcpy(&range[2], &lba_tmp, sizeof(lba_tmp));  /* starting LBA */
    }

    __sync_synchronize();

    /* Build the submission queue entry */
    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)q->sq_virt;
    uint16_t tail = q->sq_tail;

    struct nvme_sq_entry *entry = &sq[tail];
    memset(entry, 0, sizeof(struct nvme_sq_entry));
    entry->cdw0 = NVME_IO_DATASET_MANAGEMENT;
    entry->nsid = (uint32_t)ns_id;
    entry->prp1 = range_phys;
    entry->cdw10 = 0;         /* number of ranges - 1  (0 = 1 range) */
    entry->cdw11 = 1;         /* bit 0 = Deallocate */

    tail = (uint16_t)((tail + 1) % q->sq_size);
    __sync_synchronize();
    q->sq_tail = tail;
    nvme_write32(&g_nvme_ctrl, nvme_sq_doorbell(&g_nvme_ctrl, q->qid), tail);

    /* Poll for completion */
    int ret = nvme_poll_io_cq(q, 10000000);

    pmm_free_frame(range_frame);
    return ret;
}

/* ── Block device submit_fn ────────────────────────────────────────── */

/** Block device submit_fn — called by blockdev layer for I/O requests */
static int nvme_blk_submit(struct blk_request *req) {
    if (!req)
        return -EIO;

    /* Determine namespace from device ID */
    int ns_index = req->dev_id - NVME_BLOCKDEV_ID;
    if (ns_index < 0 || ns_index >= (int)g_nvme_ctrl.nn)
        return -EIO;

    uint32_t nsid = (uint32_t)(ns_index + 1);  /* namespace IDs are 1-based */

    /* ── Handle discard (TRIM) requests ───────────── */
    if (req->flags & BLK_REQ_DISCARD) {
        int ret = nvme_deallocate((int)nsid, req->lba, req->count);
        req->result = ret;
        return ret;
    }

    /* Get the I/O queue for the current CPU */
    struct nvme_io_queue *q = nvme_get_io_queue();
    if (!q || !q->valid)
        return -EIO;

    /* Allocate physically-contiguous pages for DMA */
    uint32_t nr_sectors = req->count;
    uint32_t nr_pages = (nr_sectors * 512 + 4095) / 4096;
    if (nr_pages == 0) nr_pages = 1;

    /* Allocate pages as an array of physically-contiguous frames.
     * For small transfers (1 page), use PRP1 directly.
     * For multi-page transfers, build a PRP list. */
    uint64_t *frames = (uint64_t *)kmalloc((size_t)nr_pages * sizeof(uint64_t));
    if (unlikely(!frames))
        return -ENOMEM;

    for (uint32_t i = 0; i < nr_pages; i++) {
        frames[i] = pmm_alloc_frame();
        if (unlikely(!frames[i])) {
            for (uint32_t j = 0; j < i; j++)
                pmm_free_frame(frames[j]);
            kfree(frames);
            return -ENOMEM;
        }
    }

    uint64_t data_phys = frames[0] * 4096;
    void *data_virt = PHYS_TO_VIRT((void*)(uintptr_t)data_phys);

    if (req->flags & BLK_REQ_WRITE) {
        /* For writes: copy data from request buffer to DMA buffer.
         * Data may span multiple pages, copy page by page. */
        uint64_t remaining = (uint64_t)nr_sectors * 512;
        uint64_t offset = 0;
        for (uint32_t i = 0; i < nr_pages && remaining > 0; i++) {
            void *page_virt = PHYS_TO_VIRT(frames[i] * 4096);
            uint64_t copy = remaining < 4096 ? remaining : 4096;
            memcpy(page_virt, (uint8_t *)req->buf + offset, (size_t)copy);
            offset += copy;
            remaining -= copy;
        }
    }

    /* Build PRP list if multi-page (NVMe PRP entries cannot cross page boundaries) */
    /* Max number of PRP entries that fit in one 4KB page (each entry is 8 bytes) */
    #define NVME_PRP_MAX_PER_PAGE 512

    uint64_t prp1 = data_phys;
    uint64_t prp2 = 0;
    uint64_t *prp_list = NULL;

    if (nr_pages > 1) {
        /* Guard against PRP list overflow (more pages than fit in one PRP page) */
        if ((nr_pages - 1) > NVME_PRP_MAX_PER_PAGE) {
            kprintf("[NVME] PRP list overflow: %u pages requested, max %u\n",
                    nr_pages, NVME_PRP_MAX_PER_PAGE + 1);
            for (uint32_t i = 0; i < nr_pages; i++)
                pmm_free_frame(frames[i]);
            kfree(frames);
            return -ENOSPC;
        }

        /* Allocate a dedicated page for the PRP list (do NOT reuse a data page) */
        uint64_t prp_list_frame = pmm_alloc_frame();
        if (unlikely(!prp_list_frame)) {
            for (uint32_t i = 0; i < nr_pages; i++)
                pmm_free_frame(frames[i]);
            kfree(frames);
            return -ENOMEM;
        }
        prp_list = (uint64_t *)PHYS_TO_VIRT(prp_list_frame * 4096);
        prp1 = prp_list_frame * 4096; /* PRP1 points to the PRP list */
        prp2 = data_phys;             /* PRP2 points to the first data page */

        /* Fill PRP list: entries point to each data page after the first */
        for (uint32_t i = 1; i < nr_pages; i++)
            prp_list[i - 1] = frames[i] * 4096;
    }

    /* Submit the I/O command with PRP1 and optional PRP list */
    int ret = nvme_io_submit_on_queue(q, nsid,
                                      !!(req->flags & BLK_REQ_WRITE),
                                      req->lba, nr_sectors, prp1, prp2, nr_pages);
    if (ret < 0) {
        if (prp_list)
            pmm_free_frame(VIRT_TO_PHYS(prp_list) / 4096);
        for (uint32_t i = 0; i < nr_pages; i++)
            pmm_free_frame(frames[i]);
        kfree(frames);
        return -EINVAL;
    }

    /* Poll for completion (synchronous for now) */
    ret = nvme_poll_io_cq(q, 10000000);
    if (ret == 0 && (req->flags & BLK_REQ_READ)) {
        /* For reads: copy data from DMA buffer to request buffer.
         * Data may span multiple pages, copy page by page. */
        uint64_t remaining = (uint64_t)nr_sectors * 512;
        uint64_t offset = 0;
        for (uint32_t i = 0; i < nr_pages && remaining > 0; i++) {
            void *page_virt = PHYS_TO_VIRT(frames[i] * 4096);
            uint64_t copy = remaining < 4096 ? remaining : 4096;
            memcpy((uint8_t *)req->buf + offset, page_virt, (size_t)copy);
            offset += copy;
            remaining -= copy;
        }
    }

    if (prp_list)
        pmm_free_frame(VIRT_TO_PHYS(prp_list) / 4096);
    for (uint32_t i = 0; i < nr_pages; i++)
        pmm_free_frame(frames[i]);
    kfree(frames);

    /* Check for AER completions on admin CQ after block I/O */
    nvme_aer_poll();

    return ret;
}

/* ── NVMe IRQ handler ──────────────────────────────────────────────── */

/**
 * nvme_irq_handler — acknowledges pending interrupts from the NVMe controller
 *
 * The current driver uses synchronous polling in nvme_blk_submit, but the
 * IRQ handler is registered to handle controller interrupt delivery.
 * Reading CSTS clears the interrupt condition on most implementations.
 */
static void nvme_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    /* Reading CSTS is sufficient to clear the interrupt on most NVMe controllers */
    (void)nvme_read32(&g_nvme_ctrl, NVME_REG_CSTS);
}

/* ── Initialize NVMe block devices ──────────────────────────────────── */

static int nvme_register_blockdevs(void) {
    int registered = 0;

    for (uint32_t nsid = 1; nsid <= g_nvme_ctrl.nn && nsid <= NVME_MAX_NS; nsid++) {
        struct nvme_identify_ns ns;
        memset(&ns, 0, sizeof(ns));

        if (nvme_identify_ns(nsid, &ns) < 0) {
            kprintf("[NVME] Failed to identify namespace %u\n", nsid);
            continue;
        }

        /* Determine sector size from LBA format using a local copy to avoid
         * -Waddress-of-packed-member warnings */
        uint8_t flbas = ns.flbas & 0x0F;
        uint8_t lbads = 9;  /* default to 512 if unknown */
        struct nvme_lba_format lf_local;
        if (flbas < ns.nlbaf + 1) {
            /* Each struct nvme_lba_format is 4 bytes, starting at byte 128
             * in the Identify Namespace data structure per NVMe spec. */
            const uint8_t *lba_fmt = (const uint8_t *)&ns + 128;
            memcpy(&lf_local, lba_fmt + flbas * sizeof(struct nvme_lba_format),
                   sizeof(lf_local));
            if (lf_local.ds >= 9 && lf_local.ds <= 13)
                lbads = lf_local.ds;
        }

        uint32_t sector_size = 1u << lbads;
        uint64_t nsze = ns.nsze;

        int ns_index = (int)(nsid - 1);
        g_nvme_ctrl.ns_sector_count[ns_index] = nsze;
        g_nvme_ctrl.ns_sector_size[ns_index] = sector_size;

        /* Build a device name: nvme0n1, nvme0n2, ... */
        char devname[16];
        snprintf(devname, sizeof(devname), "nvme0n%u", nsid);

        int dev_id = NVME_BLOCKDEV_ID + ns_index;
        int ret = blockdev_register(dev_id, devname,
                                    nvme_blk_submit, NULL,
                                    nsze, 0);
        if (ret < 0) {
            kprintf("[NVME] Failed to register blockdev %s\n", devname);
            continue;
        }

        g_nvme_ctrl.ns_blkdev_id[ns_index] = dev_id;
        registered++;

        /* Register multipath device for this namespace (if not already created) */
        nvme_mpath_find_or_create(nsid, 0, dev_id, nsze, sector_size);

        /* Set max transfer size based on controller MDTS (Item 328).
         * MDTS = log2(max_transfer / MPS), where MPS = 2^(mpsmin+12).
         * If mdts == 0, maximum is 1 MPS (typically 4096 bytes).
         * We compute max sectors = 2^mdts * 2^(mpsmin+12) / sector_size.
         * Clamp to a reasonable minimum of 8 sectors. */
        {
            uint32_t mps_bytes = 1u << (g_nvme_ctrl.mpsmin + 12);
            uint32_t max_xfer_bytes = mps_bytes * (1u << g_nvme_ctrl.mdts);
            uint32_t max_sectors = max_xfer_bytes / sector_size;
            if (max_sectors < 8) max_sectors = 8;
            blockdev_set_max_transfer(dev_id, max_sectors);
        }

        kprintf("[NVME] Registered namespace %u: %s (%llu sectors of %u bytes)\n",
                nsid, devname, (unsigned long long)nsze, sector_size);
    }

    return registered;
}

/* ── NVMe multipath support ──────────────────────────────────────────── */

/**
 * nvme_mpath_find_or_create — find or create a multipath device for a NSID.
 * For now (single controller), we track per-controller paths for future
 * expansion when multiple controllers are present.
 */
static struct nvme_mpath_dev *nvme_mpath_find_or_create(uint32_t nsid,
                                                         int ctrl_index,
                                                         int path_dev_id,
                                                         uint64_t sector_count,
                                                         uint32_t sector_size)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mpath_lock, &irq_flags);

    /* Look for existing multipath device for this NSID */
    struct nvme_mpath_dev *mp = NULL;
    for (int i = 0; i < g_mpath_count; i++) {
        if (g_mpath_devs[i].active && g_mpath_devs[i].nsid == nsid) {
            mp = &g_mpath_devs[i];
            break;
        }
    }

    if (!mp && g_mpath_count < NVME_MPATH_MAX_DEVS) {
        /* Create new multipath device */
        mp = &g_mpath_devs[g_mpath_count];
        memset(mp, 0, sizeof(*mp));
        mp->active = 1;
        mp->nsid = nsid;
        mp->sector_count = sector_count;
        mp->sector_size = sector_size;
        mp->rr_next = 0;

        /* Register multipath block device */
        mp->mp_dev_id = NVME_BLOCKDEV_ID + 100 + g_mpath_count;
        char mp_name[32];
        snprintf(mp_name, sizeof(mp_name), "nvme%dn%u-mpath", ctrl_index, nsid);

        int ret = blockdev_register(mp->mp_dev_id, mp_name,
                                     nvme_mpath_submit, NULL,
                                     sector_count, 0);
        if (ret == 0) {
            g_mpath_count++;
            kprintf("[NVME] Multipath device %s (id=%d) created for NSID %u\n",
                    mp_name, mp->mp_dev_id, nsid);
        } else {
            memset(mp, 0, sizeof(*mp));
            mp = NULL;
        }
    }

    /* Add path if we have room */
    if (mp && mp->nr_paths < NVME_MPATH_MAX_PATHS) {
        struct nvme_path *path = &mp->paths[mp->nr_paths];
        memset(path, 0, sizeof(*path));
        path->active = 1;
        path->ctrl_index = ctrl_index;
        path->nsid = nsid;
        path->dev_id = path_dev_id;
        mp->nr_paths++;
        kprintf("[NVME] Multipath: added path (ctrl=%d, nsid=%u, dev=%d) to NSID %u\n",
                ctrl_index, nsid, path_dev_id, nsid);
    }

    spinlock_irqsave_release(&g_mpath_lock, irq_flags);
    return mp;
}

/**
 * nvme_mpath_submit — block device submit for multipath device.
 * Uses round-robin across paths. On I/O error, retries on next path.
 */
static int nvme_mpath_submit(struct blk_request *req)
{
    if (!req) return -EINVAL;

    /* Find the multipath device from the dev_id */
    struct nvme_mpath_dev *mp = NULL;
    for (int i = 0; i < g_mpath_count; i++) {
        if (g_mpath_devs[i].active && g_mpath_devs[i].mp_dev_id == req->dev_id) {
            mp = &g_mpath_devs[i];
            break;
        }
    }
    if (!mp || mp->nr_paths == 0)
        return -ENODEV;

    uint64_t start_ticks = 0; /* timer_get_ticks() — would need timer.h */
    int first_error = 0;

    /* Round-robin across paths */
    for (int attempt = 0; attempt < mp->nr_paths && attempt < 2; attempt++) {
        int idx = mp->rr_next % mp->nr_paths;
        mp->rr_next = (mp->rr_next + 1) % mp->nr_paths;

        struct nvme_path *path = &mp->paths[idx];
        if (!path->active) continue;

        /* Submit I/O on this path (direct NVMe I/O) */
        req->dev_id = (uint8_t)path->dev_id;

        /* Call the original path's block submit */
        uint8_t saved_dev_id = req->dev_id;
        req->dev_id = (uint8_t)path->dev_id;

        int ret = -1;

        /* Route through the path's actual block device */
        if (path->dev_id >= 0) {
            ret = blk_submit_sync(path->dev_id, req->lba, req->count,
                                   req->buf, req->flags);
        }

        req->dev_id = saved_dev_id;

        /* Update per-path statistics */
        uint64_t elapsed = 0; /* timer_get_ticks() - start_ticks; */

        if (ret == 0) {
            path->stats.success_count++;
            path->stats.total_latency_ticks += elapsed;
            path->stats.last_latency_ticks = elapsed;
            req->result = 0;
            return 0;
        }

        path->stats.fail_count++;
        if (!first_error) first_error = ret;

        kprintf("[NVME] Multipath: path %d (ctrl=%d, nsid=%u) failed, retrying...\n",
                idx, path->ctrl_index, path->nsid);
    }

    req->result = first_error;
    return first_error;
}

/**
 * nvme_mpath_get_stats — get per-path statistics for a multipath device.
 * Returns 0 on success, -1 if device not found.
 */
int nvme_mpath_get_stats(int mp_dev_id,
                          int *nr_paths,
                          struct nvme_path_stats *stats_out,
                          int max_paths)
{
    for (int i = 0; i < g_mpath_count; i++) {
        if (g_mpath_devs[i].active && g_mpath_devs[i].mp_dev_id == mp_dev_id) {
            struct nvme_mpath_dev *mp = &g_mpath_devs[i];
            if (nr_paths) *nr_paths = mp->nr_paths;
            if (stats_out && max_paths > 0) {
                int n = mp->nr_paths < max_paths ? mp->nr_paths : max_paths;
                for (int j = 0; j < n; j++) {
                    stats_out[j] = mp->paths[j].stats;
                }
            }
            return 0;
        }
    }
    return -EINVAL;
}

/* ── Main initialization ───────────────────────────────────────────── */

int __init nvme_init(void) {
    if (g_nvme_init_done)
        return 0;

    memset(&g_nvme_ctrl, 0, sizeof(g_nvme_ctrl));

    /* Probe PCI */
    if (nvme_probe_pci() < 0) {
        kprintf("[NVME] No NVMe controller found\n");
        g_nvme_init_done = 1;
        return -EIO;
    }

    /* Setup admin queues */
    if (nvme_setup_admin_queues() < 0) {
        kprintf("[NVME] Failed to setup admin queues\n");
        return -EIO;
    }

    /* Enable controller */
    if (nvme_enable_controller() < 0) {
        kprintf("[NVME] Failed to enable controller\n");
        return -EIO;
    }

    /* Identify controller */
    struct nvme_identify_ctrl id;
    if (nvme_identify_ctrl(&id) == 0) {
        kprintf("[NVME] Model: %.40s  SN: %.20s\n", id.mn, id.sn);
        kprintf("[NVME] FW rev: %.8s, Namespaces: %u\n", id.fr, id.nn);
        /* Save MDTS (Maximum Data Transfer Size) for bio splitting (Item 328) */
        g_nvme_ctrl.mdts = id.mdts;
    }

    /* Set up interrupts: try MSI-X multi-vector, then MSI, then INTx */
    memset(&g_nvme_int_cfg, 0, sizeof(g_nvme_int_cfg));
    if (g_nvme_pci_saved) {
        pci_setup_interrupts(&g_nvme_pci_dev, &g_nvme_int_cfg, nvme_irq_handler);
        kprintf("[NVME] Interrupts: type=%d, %d vector(s), base=%d\n",
                g_nvme_int_cfg.type, g_nvme_int_cfg.n_vectors, g_nvme_int_cfg.vector);
    } else {
        /* Fallback: register legacy INTx handler directly */
        idt_register_handler((uint8_t)(32 + g_nvme_ctrl.irq), nvme_irq_handler);
    }

    /* Set up per-CPU I/O queue pairs */
    if (nvme_setup_io_queues() < 0) {
        kprintf("[NVME] I/O queue setup failed\n");
    }

    /* Register namespaces as block devices */
    if (g_nvme_ctrl.nn > 0) {
        int reg = nvme_register_blockdevs();
        if (reg > 0)
            kprintf("[NVME] Registered %d namespace(s)\n", reg);
        else
            kprintf("[NVME] No namespaces registered\n");
    }

    g_nvme_init_done = 1;
    kprintf("[NVME] Driver initialized\n");

    /* Start AER monitoring for asynchronous events */
    nvme_aer_init();

    return 0;
}

/* ── Utility functions ─────────────────────────────────────────────── */
#ifndef MODULE
#include "initcall.h"
device_initcall(nvme_init);
#endif

int nvme_is_present(void) {
    return g_nvme_ctrl.present;
}

void nvme_print_info(void) {
    if (!g_nvme_ctrl.present) {
        kprintf("NVMe: Not present\n");
        return;
    }
    kprintf("NVMe: present, regs at 0x%llX, max_q_depth=%u, namespaces=%u\n",
            (unsigned long long)g_nvme_ctrl.phys_regs,
            g_nvme_ctrl.max_q_depth,
            g_nvme_ctrl.nn);
    kprintf("NVMe: %u I/O queue(s) active\n", g_nvme_ctrl.nr_io_queues);
    for (uint32_t i = 0; i < g_nvme_ctrl.nr_io_queues; i++) {
        if (g_nvme_ctrl.io_queues[i].valid) {
            kprintf("  Queue %u (qid=%u): CPU %u\n",
                    i, g_nvme_ctrl.io_queues[i].qid,
                    g_nvme_ctrl.io_queues[i].cpu_id);
        }
    }
}

/* ── Cleanup / module exit ─────────────────────────────────────────── */

/**
 * nvme_exit — shut down NVMe controller and free resources.
 *
 * Disables the controller, frees all allocated DMA buffers (admin and I/O
 * queues), and resets driver state so the module can be safely unloaded.
 */
static void nvme_exit(void)
{
    if (!g_nvme_ctrl.present || !g_nvme_init_done)
        return;

    kprintf("[NVME] Shutting down...\n");

    /* Stop AER monitoring */
    nvme_aer_exit();

    /* Disable PCI interrupts based on active type */
    if (g_nvme_pci_saved) {
        if (g_nvme_int_cfg.type == 2)
            pci_disable_msix(&g_nvme_pci_dev);
        else if (g_nvme_int_cfg.type == 1)
            pci_disable_msi(&g_nvme_pci_dev);
        /* INTx (type 0) has no disable needed at the PCI level */
    }

    /* Disable the controller: clear CC.EN */
    uint32_t cc = nvme_read32(&g_nvme_ctrl, NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    nvme_write32(&g_nvme_ctrl, NVME_REG_CC, cc);

    /* Wait for CSTS.RDY = 0 (controller shutdown) */
    nvme_wait_ready(&g_nvme_ctrl, 0, 2000);

    /* Free admin queue pages */
    if (g_nvme_ctrl.admin_sq_phys) {
        pmm_free_frame(g_nvme_ctrl.admin_sq_phys / 4096);
        g_nvme_ctrl.admin_sq_phys = 0;
    }
    if (g_nvme_ctrl.admin_cq_phys) {
        pmm_free_frame(g_nvme_ctrl.admin_cq_phys / 4096);
        g_nvme_ctrl.admin_cq_phys = 0;
    }
    g_nvme_ctrl.admin_sq = NULL;
    g_nvme_ctrl.admin_cq = NULL;

    /* Free I/O queue pages */
    for (uint32_t i = 0; i < g_nvme_ctrl.nr_io_queues; i++) {
        struct nvme_io_queue *q = &g_nvme_ctrl.io_queues[i];
        if (!q->valid)
            continue;
        if (q->sq_phys) {
            pmm_free_frame(q->sq_phys / 4096);
            q->sq_phys = 0;
        }
        if (q->cq_phys) {
            pmm_free_frame(q->cq_phys / 4096);
            q->cq_phys = 0;
        }
        q->sq_virt = NULL;
        q->cq_virt = NULL;
        q->valid = 0;
    }
    g_nvme_ctrl.nr_io_queues = 0;

    /* Unregister block devices (best-effort) */
    for (uint32_t nsid = 1; nsid <= g_nvme_ctrl.nn && nsid <= NVME_MAX_NS; nsid++) {
        int ns_index = (int)(nsid - 1);
        int dev_id = g_nvme_ctrl.ns_blkdev_id[ns_index];
        if (dev_id >= 0)
            blockdev_unregister(dev_id);
        g_nvme_ctrl.ns_blkdev_id[ns_index] = -1;
    }

    /* Reset driver state */
    g_nvme_ctrl.present = 0;
    g_nvme_ctrl.nn = 0;
    g_nvme_init_done = 0;

    kprintf("[NVME] Driver shut down\n");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Power Management API (PS Profiles)
 *
 *  Implements NVMe power management via Get/Set Features (FID 0x02)
 *  and Autonomous Power State Transition (FID 0x0C).
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Get number of supported power states ──────────────────────────── */

int nvme_get_power_state_count(void)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.power_states_parsed)
        return 0;
    return (int)g_nvme_ctrl.npss;
}

/* ── Get a power state descriptor by index ─────────────────────────── */

int nvme_get_power_state_desc(int ps_index, struct nvme_power_state_desc *desc)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.power_states_parsed || !desc)
        return -EINVAL;
    if (ps_index < 0 || ps_index >= (int)g_nvme_ctrl.npss)
        return -EINVAL;
    memcpy(desc, &g_nvme_ctrl.power_states[ps_index],
           sizeof(struct nvme_power_state_desc));
    return 0;
}

/* ── Transition to a specific power state via Set Features (FID 0x02) ── */

int nvme_set_power_state(int ps_index)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.power_states_parsed)
        return -EIO;
    if (ps_index < 0 || ps_index >= (int)g_nvme_ctrl.npss)
        return -EINVAL;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;                               /* No data transfer */
    cmd.cdw10 = NVME_FEAT_POWER_MANAGEMENT;     /* Feature Identifier */
    cmd.cdw11 = (uint32_t)(ps_index & 0x1F);    /* Bits [4:0] = PS number */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        uint16_t status = cqe.status;
        if (status & 0x0001)
            return -EIO;
        kprintf("[NVME] Power state transition to PS%d requested\n", ps_index);
        return 0;
    }
    return ret;
}

/* ── Get current power state via Get Features (FID 0x02) ────────────── */

int nvme_get_current_power_state(void)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_GET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;                               /* No data transfer */
    cmd.cdw10 = NVME_FEAT_POWER_MANAGEMENT;     /* Feature Identifier */
    cmd.cdw11 = 0;                               /* SEL = 0 (current value) */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        uint16_t status = cqe.status;
        if (status & 0x0001)
            return -EIO;
        /* Current power state number is in bits [4:0] of cdw0 */
        return (int)(cqe.cdw0 & 0x1F);
    }
    return ret;
}

/* ── Enable/disable Autonomous Power State Transition (FID 0x0C) ────── */

int nvme_set_apst(int enable, struct nvme_apst_entry *table, int nr_entries)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.nsid = 0;
    cmd.cdw10 = NVME_FEAT_AUTO_POWER_STATE_TRANSITION;

    if (enable && table && nr_entries > 0) {
        /* Allocate a page for the APST table (max 32 × 8 bytes = 256 bytes) */
        uint64_t data_frame = pmm_alloc_frame();
        if (unlikely(!data_frame))
            return -ENOMEM;

        uint64_t data_phys = data_frame * 4096;
        struct nvme_apst_entry *apst =
            (struct nvme_apst_entry *)PHYS_TO_VIRT((void*)(uintptr_t)data_phys);
        memset(apst, 0, 4096);

        int n = nr_entries < 32 ? nr_entries : 32;
        for (int i = 0; i < n; i++)
            apst[i] = table[i];

        __sync_synchronize();

        cmd.prp1 = data_phys;
        cmd.cdw11 = 1;          /* APSTE = 1 (enable) */

        int ret = nvme_submit_admin_cmd(&cmd, &cqe);
        pmm_free_frame(data_frame);

        if (ret == 0) {
            kprintf("[NVME] APST enabled (%d entries)\n", n);
            return 0;
        }
        return ret;
    }

    /* Disable APST — no data transfer needed */
    cmd.prp1 = 0;
    cmd.cdw11 = 0;              /* APSTE = 0 (disable) */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        kprintf("[NVME] APST disabled\n");
        return 0;
    }
    return ret;
}

/* ── Print all power state descriptors for diagnostics ──────────────── */

void nvme_print_power_states(void)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.power_states_parsed) {
        kprintf("NVMe: Power states not available\n");
        return;
    }

    kprintf("NVMe Power States (%d supported):\n", g_nvme_ctrl.npss);
    for (int i = 0; i < (int)g_nvme_ctrl.npss; i++) {
        const struct nvme_power_state_desc *ps = &g_nvme_ctrl.power_states[i];
        int non_op = (ps->mp & 0x8000) ? 1 : 0;
        uint16_t mp_mw = (uint16_t)(ps->mp & 0x7FFF);  /* Max Power in centiwatts */

        kprintf("  PS%2d: %s  max_pwr=%u.%02uW  enlat=%uus  exlat=%uus  "
                "rrt=%u rrl=%u rwt=%u rwl=%u\n",
                i,
                non_op ? "NON-OP" : "ACTIVE",
                (unsigned)(mp_mw / 100), (unsigned)(mp_mw % 100),
                ps->enlat, ps->exlat,
                (unsigned)ps->rrt, (unsigned)ps->rrl,
                (unsigned)ps->rwt, (unsigned)ps->rwl);

        /* Idle power information */
        if (ps->idle_power & 0x0020) {
            /* Scale = watts */
            uint16_t idle_mw = (uint16_t)((ps->idle_power >> 6) * 100);
            unsigned idle_time = (unsigned)(ps->idle_power & 0x1F) * 100; /* ms */
            kprintf("       idle_pwr=%u.%02uW  idle_time=%ums\n",
                    idle_mw / 100, idle_mw % 100, idle_time);
        } else if (ps->idle_power & 0xFC00) {
            /* Scale = centiwatts */
            unsigned idle_cmw = (unsigned)(ps->idle_power >> 6);
            unsigned idle_time = (unsigned)(ps->idle_power & 0x1F) * 100; /* ms */
            kprintf("       idle_pwr=%u.%02uW  idle_time=%ums\n",
                    idle_cmw / 100, idle_cmw % 100, idle_time);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Predictable Latency Mode (FID 0x09, 0x0A)
 *
 *  NVMe Predictable Latency Mode allows the host to request deterministic
 *  latency behavior from the controller.  When enabled with DTYPE=1
 *  (deterministic), the controller provides bounded latency for I/O
 *  operations at the cost of throughput.  When enabled with DTYPE=2
 *  (non-deterministic), the controller optimises for throughput.
 *
 *  Feature IDs used:
 *    0x09 — Predictable Latency Mode Config  (Get/Set Features)
 *    0x0A — Predictable Latency Mode Window  (Get Features, read-only)
 *
 *  References:
 *    NVMe Base Specification Revision 1.4, Sections 5.21.1.2–5.21.1.8
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Set predictable latency mode configuration (FID 0x09) ──────────── */

int nvme_set_predictable_latency(int dtype)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    if (dtype < NVME_PLM_DTYPE_DISABLED || dtype > NVME_PLM_DTYPE_NON_DETERMINISTIC)
        return -EINVAL;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;                               /* No data transfer */
    cmd.cdw10 = NVME_FEAT_PREDICTABLE_LATENCY_CONFIG;
    cmd.cdw11 = (uint32_t)(dtype & 0x3);         /* DTYPE in bits [1:0] */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        uint16_t status = cqe.status;
        if (status & 0x0001)
            return -EIO;
        const char *dtype_name;
        switch (dtype) {
        case NVME_PLM_DTYPE_DETERMINISTIC:
            dtype_name = "deterministic";
            break;
        case NVME_PLM_DTYPE_NON_DETERMINISTIC:
            dtype_name = "non-deterministic";
            break;
        default:
            dtype_name = "disabled";
            break;
        }
        kprintf("[NVME] Predictable latency mode set to '%s' (DTYPE=%d)\n",
                dtype_name, dtype);
        return 0;
    }
    return ret;
}

/* ── Get predictable latency mode configuration (FID 0x09) ──────────── */

int nvme_get_predictable_latency(void)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_GET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;                               /* No data transfer */
    cmd.cdw10 = NVME_FEAT_PREDICTABLE_LATENCY_CONFIG;
    cmd.cdw11 = 0;                               /* SEL = 0 (current value) */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        uint16_t status = cqe.status;
        if (status & 0x0001)
            return -EIO;
        /* Current DTYPE value is in bits [1:0] of cdw0 */
        return (int)(cqe.cdw0 & 0x3);
    }
    return ret;
}

/* ── Get predictable latency window status (FID 0x0A) ───────────────── */

int nvme_get_predictable_latency_window(void)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_GET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = 0;                               /* No data transfer */
    cmd.cdw10 = NVME_FEAT_PREDICTABLE_LATENCY_WINDOW;
    cmd.cdw11 = 0;                               /* SEL = 0 (current value) */

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret == 0) {
        uint16_t status = cqe.status;
        if (status & 0x0001)
            return -EIO;
        /* Window type is in bits [1:0] of cdw0 */
        return (int)(cqe.cdw0 & 0x3);
    }
    return ret;
}

/* ── Print predictable latency mode diagnostics ─────────────────────── */

void nvme_print_predictable_latency(void)
{
    if (!g_nvme_ctrl.present) {
        kprintf("NVMe: Predictable latency mode — controller not present\n");
        return;
    }

    int dtype = nvme_get_predictable_latency();
    if (dtype < 0) {
        kprintf("NVMe: Predictable latency mode — query failed (err=%d)\n", dtype);
        return;
    }

    const char *dtype_name;
    switch (dtype) {
    case NVME_PLM_DTYPE_DETERMINISTIC:
        dtype_name = "Deterministic";
        break;
    case NVME_PLM_DTYPE_NON_DETERMINISTIC:
        dtype_name = "Non-Deterministic";
        break;
    default:
        dtype_name = "Disabled";
        break;
    }

    kprintf("NVMe: Predictable Latency Mode — %s (DTYPE=%d)\n", dtype_name, dtype);

    int window = nvme_get_predictable_latency_window();
    if (window >= 0) {
        const char *window_name;
        switch (window) {
        case NVME_PLM_WINDOW_DETERMINISTIC:
            window_name = "Deterministic";
            break;
        case NVME_PLM_WINDOW_NON_DETERMINISTIC:
            window_name = "Non-Deterministic";
            break;
        default:
            window_name = "None";
            break;
        }
        kprintf("NVMe:   Current Window  — %s (type=%d)\n", window_name, window);
    } else {
        kprintf("NVMe:   Current Window  — query failed (err=%d)\n", window);
    }

    int ps = nvme_get_current_power_state();
    if (ps >= 0)
        kprintf("NVMe:   Power State    — PS%d\n", ps);
}

/* ── Module entry/exit points ─────────────────────────────────────── */

#ifdef MODULE
int init_module(void) {
    return nvme_init();
}

void cleanup_module(void) {
    nvme_exit();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("NVM Express (NVMe) PCIe SSD driver with per-CPU I/O queue pairs");
MODULE_ALIAS("pci:v00008086d0000F1A5sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000F1A6sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001AF4d00005841sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001AF4d00005842sv*sd*bc*sc*i*");
MODULE_VERSION("1.0");
#endif /* MODULE */

/* ═══════════════════════════════════════════════════════════════════════
 *  Asynchronous Event Request (AER) — Continuous event monitoring
 *
 *  AER allows the NVMe controller to asynchronously notify the host of
 *  events (errors, SMART thresholds, vendor-specific notifications, etc.)
 *  without polling.  The host submits an AER admin command; the controller
 *  holds it outstanding and completes it when an event occurs.  The host
 *  must re-submit after each completion for continuous monitoring.
 *
 *  Per NVMe Base Spec Revision 1.4, Figure 107 — Event types:
 *    0x01  Error Events
 *    0x02  SMART / Health Events
 *    0x03  Command Set Dependent Notifications
 *    0x04  Vendor Specific Events
 *
 *  We use CID 0xAAAA to identify AER completions on the shared admin CQ.
 * ═══════════════════════════════════════════════════════════════════════ */

#define NVME_AER_CID        0xAAAA

/* ── Non-blocking AER submission ───────────────────────────────────── */

static int nvme_aer_submit(void)
{
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.admin_sq)
        return -EIO;

    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)g_nvme_ctrl.admin_sq;
    uint16_t tail = g_nvme_ctrl.admin_sq_tail;

    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_ASYNC_EVENT | ((uint32_t)NVME_AER_CID << 16);

    memcpy(&sq[tail], &cmd, sizeof(cmd));
    tail = (uint16_t)((tail + 1) % 64);
    g_nvme_ctrl.admin_sq_tail = tail;

    __sync_synchronize();
    nvme_write32(&g_nvme_ctrl, nvme_sq_doorbell(&g_nvme_ctrl, 0), tail);

    g_aer_pending = 1;
    kprintf("[NVME] AER submitted (SQ tail=%u)\n", tail);
    return 0;
}

/* ── Handle an AER completion event ────────────────────────────────── */

static void nvme_aer_handle_event(uint32_t cdw0, uint16_t status)
{
    uint8_t event_type = (uint8_t)(cdw0 & 0xFF);
    uint16_t event_info = (uint16_t)((cdw0 >> 8) & 0x7FFF);
    int log_page = (int)((cdw0 >> 23) & 1);
    uint8_t sc = (uint8_t)((status >> 1) & 0xFF);
    uint8_t sct = (uint8_t)((status >> 9) & 0x7);

    kprintf("[NVME] AER completion: type=0x%02X info=0x%04X "
            "log_page=%d SCT=%u SC=%u\n",
            event_type, event_info, log_page,
            (unsigned)sct, (unsigned)sc);

    switch (event_type) {
    case NVME_AER_TYPE_ERROR:
        switch (event_info) {
        case NVME_AER_ERR_SQ:
            kprintf("[NVME]   -> Submission Queue error\n");
            break;
        case NVME_AER_ERR_INVALID_DB:
            kprintf("[NVME]   -> Invalid Doorbell Write\n");
            break;
        case NVME_AER_ERR_DIAG_FAIL:
            kprintf("[NVME]   -> Diagnostic Failure\n");
            break;
        case NVME_AER_ERR_PERSIST_INTERNAL:
            kprintf("[NVME]   -> Persistent Internal Error\n");
            break;
        default:
            kprintf("[NVME]   -> Error event (info=0x%04X)\n", event_info);
            break;
        }
        break;

    case NVME_AER_TYPE_SMART:
        switch (event_info) {
        case NVME_AER_SMART_RELIABILITY:
            kprintf("[NVME]   -> NVM Subsystem Reliability warning\n");
            break;
        case NVME_AER_SMART_TEMP_THRESH:
            kprintf("[NVME]   -> Temperature threshold exceeded\n");
            break;
        case NVME_AER_SMART_SPARE_THRESH:
            kprintf("[NVME]   -> Spare capacity below threshold\n");
            break;
        default:
            kprintf("[NVME]   -> SMART event (info=0x%04X)\n", event_info);
            break;
        }
        break;

    case NVME_AER_TYPE_CMDSET:
        kprintf("[NVME]   -> Command Set Dependent notification\n");
        break;

    case NVME_AER_TYPE_VENDOR:
        kprintf("[NVME]   -> Vendor Specific event\n");
        break;

    default:
        kprintf("[NVME]   -> Unknown event type 0x%02X\n", event_type);
        break;
    }
}

/* ── Check admin CQ for AER completion ─────────────────────────────── */

void nvme_aer_poll(void)
{
    if (!g_aer_pending || !g_nvme_ctrl.present)
        return;

    struct nvme_cq_entry *cq = (struct nvme_cq_entry *)g_nvme_ctrl.admin_cq;
    uint16_t head = g_nvme_ctrl.admin_cq_head;

    /* Check if a completion is available at head */
    if (cq[head].status == 0xFFFF)
        return;

    /* Read completion entry */
    uint32_t cdw0 = cq[head].cdw0;
    uint16_t cid = cq[head].cid;
    uint16_t status = cq[head].status;

    /* Only consume if this is our AER completion */
    if (cid != NVME_AER_CID)
        return;

    /* Mark completion as consumed */
    cq[head].status = 0xFFFF;
    head = (uint16_t)((head + 1) % 64);
    g_nvme_ctrl.admin_cq_head = head;

    /* Re-arm admin CQ doorbell */
    nvme_write32(&g_nvme_ctrl, nvme_cq_doorbell(&g_nvme_ctrl, 0), head);

    g_aer_pending = 0;

    /* Handle the event */
    nvme_aer_handle_event(cdw0, status);

    /* Re-submit AER for continuous event monitoring */
    nvme_aer_submit();
}

/* ── Initialize AER monitoring ─────────────────────────────────────── */

int __init nvme_aer_init(void)
{
    if (!g_nvme_ctrl.present)
        return -EIO;

    kprintf("[NVME] Starting asynchronous event requests (AER)...\n");
    return nvme_aer_submit();
}

/* ── Shut down AER monitoring ──────────────────────────────────────── */

void nvme_aer_exit(void)
{
    g_aer_pending = 0;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Submit command to a queue ──────────────────────────── */
static int nvme_submit_cmd(void *q, struct nvme_sq_entry *cmd)
{
    if (!q || !cmd)
        return -EINVAL;

    struct nvme_io_queue *queue = (struct nvme_io_queue *)q;
    if (!queue->valid || !queue->sq_virt)
        return -ENODEV;

    /* Copy command to the submission queue slot */
    struct nvme_sq_entry *slot = (struct nvme_sq_entry *)queue->sq_virt + queue->sq_tail;
    memcpy(slot, cmd, sizeof(struct nvme_sq_entry));

    uint16_t sq_tail = queue->sq_tail;
    queue->sq_tail = (uint16_t)((queue->sq_tail + 1) % queue->sq_size);

    /* Ring the submission queue doorbell */
    uint32_t doorbell_offset = 0x1000 + (2 * queue->qid) * queue->stride;
    nvme_write32(&g_nvme_ctrl, doorbell_offset, queue->sq_tail);

    return 0;
}

/* ── Poll for command completion ───────────────────────── */
static int nvme_complete_cmd(void *q, struct nvme_cq_entry *cqe)
{
    if (!q || !cqe)
        return -EINVAL;

    struct nvme_io_queue *queue = (struct nvme_io_queue *)q;
    if (!queue->valid || !queue->cq_virt)
        return -ENODEV;

    /* Poll for phase tag change */
    struct nvme_cq_entry *slot = (struct nvme_cq_entry *)queue->cq_virt + queue->cq_head;
    uint16_t expected_phase = 1;  /* initial phase tag */
    uint32_t timeout = 10000000;

    while (timeout--) {
        uint16_t status = slot->status;
        uint16_t phase = (status >> 14) & 1;
        if (phase == expected_phase)
            break;
        __asm__ volatile("pause");
    }

    if (timeout == 0)
        return -EIO;

    /* Copy completion entry */
    memcpy(cqe, slot, sizeof(struct nvme_cq_entry));

    /* Advance completion queue head */
    queue->cq_head = (uint16_t)((queue->cq_head + 1) % queue->cq_size);

    /* Ring the completion queue doorbell */
    uint32_t doorbell_offset = 0x1000 + (2 * queue->qid + 1) * queue->stride;
    nvme_write32(&g_nvme_ctrl, doorbell_offset, queue->cq_head);

    return 0;
}

/* ── Create completion queue (admin command) ──────────── */
static int nvme_create_cq(uint16_t cqid, uint64_t addr, uint16_t size, uint16_t iv)
{
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    /* cdw10: bits [31] PC=1 (physically contiguous), [16] IEN=1 (IRQ enabled), [15:0] CQID */
    cmd.cdw10 = (1u << 31) | (1u << 16) | (uint32_t)(cqid & 0xFFFF);
    /* cdw11: bits [31:16] queue size (1-based), [15:0] interrupt vector */
    cmd.cdw11 = ((uint32_t)((size - 1) & 0xFFFF) << 16) | (iv & 0xFFFF);
    cmd.prp1 = addr;

    return nvme_submit_admin_cmd(&cmd, &cqe);
}

/* ── Create submission queue (admin command) ──────────── */
static int nvme_create_sq(uint16_t sqid, uint64_t addr, uint16_t size, uint16_t cqid)
{
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));
    memset(&cqe, 0, sizeof(cqe));

    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    /* cdw10: bits [31] PC=1 (physically contiguous), [15:0] SQID */
    cmd.cdw10 = (1u << 31) | (uint32_t)(sqid & 0xFFFF);
    /* cdw11: bits [31:16] queue size (1-based), [15:0] CQID */
    cmd.cdw11 = ((uint32_t)((size - 1) & 0xFFFF) << 16) | (cqid & 0xFFFF);
    cmd.prp1 = addr;

    return nvme_submit_admin_cmd(&cmd, &cqe);
}

/* ── Create I/O queue pair (CQ + SQ) ────────────────── */
/* Forward declarations for queue delete admin commands */
int nvme_delete_cq(uint16_t cqid);
int nvme_delete_sq(uint16_t sqid);

static int nvme_create_io_queue_pair(uint16_t qid, uint64_t cq_addr, uint64_t sq_addr,
                               uint16_t cq_size, uint16_t sq_size,
                               uint16_t irq_vector)
{
    /* Create I/O Completion Queue first */
    int ret = nvme_create_cq(qid, cq_addr, cq_size, irq_vector);
    if (ret < 0) {
        kprintf("[NVME] nvme_create_io_queue_pair: failed to create CQ %u\n",
                (unsigned int)qid);
        return ret;
    }

    /* Create I/O Submission Queue (associated with the same CQID) */
    ret = nvme_create_sq(qid, sq_addr, sq_size, qid);
    if (ret < 0) {
        kprintf("[NVME] nvme_create_io_queue_pair: failed to create SQ %u, ",
                (unsigned int)qid);
        kprintf("rolling back CQ %u\n", (unsigned int)qid);
        /* Rollback: delete the CQ on SQ creation failure */
        nvme_delete_cq(qid);
        return ret;
    }

    return 0;
}

/* ── Delete completion queue (admin command) ──────────── */
int nvme_delete_cq(uint16_t cqid)
{
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_DELETE_CQ | (0 << 8);
    cmd.cdw10 = cqid;

    return nvme_submit_admin_cmd(&cmd, &cqe);
}

/* ── Delete submission queue (admin command) ──────────── */
int nvme_delete_sq(uint16_t sqid)
{
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cqe;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_DELETE_SQ | (0 << 8);
    cmd.cdw10 = sqid;

    return nvme_submit_admin_cmd(&cmd, &cqe);
}
