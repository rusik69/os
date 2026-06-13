/*
 * nvme.c — NVMe (NVM Express) PCI driver with multi-queue support
 *
 * Features:
 *   - Per-CPU I/O submission/completion queue pairs (qid = CPU index + 1)
 *   - Block device registration via blockdev layer
 *   - Proper doorbell stride handling
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
#include "idt.h"
#include "apic.h"
#ifdef MODULE
#include "module.h"
#endif

static struct nvme_ctrl g_nvme_ctrl;
static int g_nvme_init_done = 0;

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
    return -1;
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
        int ret2 = pci_find_class(NVME_PCI_CLASS, (int)sub_prog, &pci2);
        if (ret2 < 0)
            return -1;
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
            return -1;
        g_nvme_ctrl.phys_regs = mmio_base;
        g_nvme_ctrl.regs = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);
    } else {
        /* I/O BAR not supported for NVMe */
        return -1;
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

    kprintf("[NVMe] Found controller: VID=0x%04X DID=0x%04X IRQ=%d\n",
            pci.vendor_id, pci.device_id, pci.irq);
    kprintf("[NVMe] Version %d.%d.%d, max queue depth %u, doorbell stride %u\n",
            (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF,
            g_nvme_ctrl.max_q_depth, g_nvme_ctrl.doorbell_stride);

    g_nvme_ctrl.present = 1;
    return 0;
}

/* ── Admin queue setup ─────────────────────────────────────────────── */

static int nvme_setup_admin_queues(void) {
    /* Allocate one page for admin SQ and one for admin CQ */
    uint64_t sq_frame = pmm_alloc_frame();
    uint64_t cq_frame = pmm_alloc_frame();
    if (!sq_frame || !cq_frame) {
        if (sq_frame) pmm_free_frame(sq_frame);
        return -1;
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
        kprintf("[NVMe] Timeout waiting for CSTS.RDY=0\n");
        return -1;
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
        kprintf("[NVMe] Timeout waiting for CSTS.RDY=1\n");
        return -1;
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
        return -1;

    if (action < NVME_SANITIZE_ACTION_BLOCK_ERASE ||
        action > NVME_SANITIZE_ACTION_CRYPTO_ERASE)
        return -1;

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

    kprintf("[NVMe] Submitting sanitize command (action=%d, overwrite_passes=%d)...\n",
            action, (action == NVME_SANITIZE_ACTION_OVERWRITE) ? overwrite_pass_count : 0);

    int ret = nvme_submit_admin_cmd(&cmd, &cqe);
    if (ret != 0) {
        kprintf("[NVMe] Sanitize command submission FAILED (timeout or error)\n");
        return -1;
    }

    /* Check completion status */
    uint16_t status = cqe.status;
    if (status & 0x0001) {
        /* Bit 0 set = error */
        uint8_t sc  = (uint8_t)((status >> 1) & 0xFF);  /* Status Code */
        uint8_t sct = (uint8_t)((status >> 9) & 0x7);   /* Status Code Type */
        kprintf("[NVMe] Sanitize command rejected: SCT=%u SC=%u\n",
                (unsigned)sct, (unsigned)sc);
        return -1;
    }

    kprintf("[NVMe] Sanitize operation ACCEPTED by controller — running in background.\n"
            "        Use 'nvme sanitize-status' to check progress.\n");
    return 0;
}

/* ── Admin command submit ──────────────────────────────────────────── */

int nvme_submit_admin_cmd(struct nvme_sq_entry *cmd, struct nvme_cq_entry *cqe) {
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.admin_sq)
        return -1;

    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)g_nvme_ctrl.admin_sq;
    uint16_t tail = g_nvme_ctrl.admin_sq_tail;

    /* Copy command to submission queue */
    memcpy(&sq[tail], cmd, sizeof(struct nvme_sq_entry));
    tail = (tail + 1) % 64;
    g_nvme_ctrl.admin_sq_tail = tail;

    /* Ring the admin SQ doorbell (qid=0) */
    uint32_t doorbell_offset = (uint32_t)nvme_sq_doorbell(&g_nvme_ctrl, 0);
    nvme_write32(&g_nvme_ctrl, doorbell_offset, tail);

    /* Spin-wait for completion */
    struct nvme_cq_entry *cq = (struct nvme_cq_entry *)g_nvme_ctrl.admin_cq;
    uint16_t head = g_nvme_ctrl.admin_cq_head;
    uint32_t timeout = 1000000;
    while (timeout--) {
        if (cq[head].status != 0xFFFF) {
            /* Got completion */
            if (cqe)
                memcpy(cqe, &cq[head], sizeof(struct nvme_cq_entry));
            /* Mark as consumed */
            cq[head].status = 0xFFFF;
            head = (head + 1) % 64;
            g_nvme_ctrl.admin_cq_head = head;

            /* Ring the admin CQ doorbell to re-arm */
            nvme_write32(&g_nvme_ctrl, nvme_cq_doorbell(&g_nvme_ctrl, 0), head);

            return 0;
        }
        __asm__ volatile("pause");
    }

    return -1;
}

/* ── Identify controller ──────────────────────────────────────────── */

int nvme_identify_ctrl(struct nvme_identify_ctrl *id) {
    if (!g_nvme_ctrl.present || !id)
        return -1;

    /* Allocate a page for the identify data (physical contiguous) */
    uint64_t data_frame = pmm_alloc_frame();
    if (!data_frame) return -1;

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
        g_nvme_ctrl.sq_entry_size = 1 << (id->sqes & 0x0F);
        g_nvme_ctrl.cq_entry_size = 1 << ((id->cqes >> 4) & 0x0F);
    } else {
        kprintf("[NVMe] Identify controller command failed\n");
    }

    pmm_free_frame(data_frame);
    return ret;
}

/* ── Identify namespace ────────────────────────────────────────────── */

static int nvme_identify_ns(uint32_t nsid, struct nvme_identify_ns *id) {
    if (!g_nvme_ctrl.present || !id || nsid == 0)
        return -1;

    uint64_t data_frame = pmm_alloc_frame();
    if (!data_frame) return -1;

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
    uint64_t data_frame = pmm_alloc_frame();
    if (!data_frame) return -1;

    memset(PHYS_TO_VIRT((void*)(uintptr_t)(data_frame * 4096)), 0, 4096);

    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.nsid = 0;
    cmd.prp1 = data_frame * 4096;
    /* cdw10: feature identifier (NumberOfQueues) */
    cmd.cdw10 = NVME_FEAT_NUMBER_OF_QUEUES;
    /* cdw11: number of I/O submission queues requested (lower 16 bits) */
    /*         and completion queues (upper 16 bits) — 0-based values. */
    uint32_t queues = (nr_queues << 16) | nr_queues;
    cmd.cdw11 = queues;

    struct nvme_cq_entry cqe;
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
            kprintf("[NVMe] Requested %u I/O queues, granted %u\n",
                    nr_queues, actual);
        }
        ret = (int)actual;
    }

    pmm_free_frame(data_frame);
    return ret;
}

/* ── I/O queue helpers ─────────────────────────────────────────────── */

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
    if (!cq_frame) return -1;

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
        kprintf("[NVMe] Create I/O CQ %u failed\n", q->qid);
        pmm_free_frame(cq_frame);
        q->cq_virt = NULL;
        q->cq_phys = 0;
        return -1;
    }

    return 0;
}

/** Create an I/O submission queue (associated with a CQ) */
static int nvme_create_io_sq(struct nvme_io_queue *q) {
    struct nvme_sq_entry cmd;
    memset(&cmd, 0, sizeof(cmd));

    /* Allocate a physically contiguous page for the SQ */
    uint64_t sq_frame = pmm_alloc_frame();
    if (!sq_frame) {
        return -1;
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
        kprintf("[NVMe] Create I/O SQ %u failed\n", q->qid);
        pmm_free_frame(sq_frame);
        q->sq_virt = NULL;
        q->sq_phys = 0;
        return -1;
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
        kprintf("[NVMe] Failed to negotiate queue count, using 1\n");
        nr_queues = 1;
    }
    if (nr_queues > nr_cpus) nr_queues = nr_cpus;

    kprintf("[NVMe] Setting up %d I/O queue pairs\n", nr_queues);

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
        q->irq_vector = i;  /* use distinct IRQ vectors for MSI-X */

        /* Create completion queue first (SQ depends on CQ) */
        if (nvme_create_io_cq(q) < 0) {
            kprintf("[NVMe] Failed to create I/O CQ %d\n", q->qid);
            continue;
        }

        /* Then create submission queue associated with this CQ */
        if (nvme_create_io_sq(q) < 0) {
            kprintf("[NVMe] Failed to create I/O SQ %d\n", q->qid);
            /* Try to clean up CQ (best-effort) */
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

    kprintf("[NVMe] %d/%d I/O queue pairs active\n", created, nr_queues);
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
        return -1;

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
                return -1;
            }
            return 0;
        }
        __asm__ volatile("pause");
    }

    return -1;  /* timeout */
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
        return -1;

    struct nvme_io_queue *q = nvme_get_io_queue();
    if (!q || !q->valid)
        return -1;

    /* Allocate one physically-contiguous page for the DSM range list.
     * The NVMe Dataset Management range is a 16-byte structure.
     * We send a single range per command. */
    uint64_t range_frame = pmm_alloc_frame();
    if (!range_frame)
        return -1;

    uint64_t range_phys = range_frame * 4096;
    uint32_t *range = (uint32_t *)PHYS_TO_VIRT(range_phys);

    /* Build the DSM range entry:
     *   bytes 0-3:  Context attributes (0 = no context)
     *   bytes 4-7:  Length (number of LBAs - 1, 0-based)
     *   bytes 8-15: Starting LBA */
    range[0] = 0;                                      /* attributes */
    range[1] = count - 1;                               /* length (0-based) */
    *(uint64_t *)&range[2] = lba;                       /* starting LBA */

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
        return -1;

    /* Determine namespace from device ID */
    int ns_index = req->dev_id - NVME_BLOCKDEV_ID;
    if (ns_index < 0 || ns_index >= (int)g_nvme_ctrl.nn)
        return -1;

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
        return -1;

    /* Allocate physically-contiguous pages for DMA */
    uint32_t nr_sectors = req->count;
    uint32_t nr_pages = (nr_sectors * 512 + 4095) / 4096;
    if (nr_pages == 0) nr_pages = 1;

    /* Allocate pages as an array of physically-contiguous frames.
     * For small transfers (1 page), use PRP1 directly.
     * For multi-page transfers, build a PRP list. */
    uint64_t *frames = (uint64_t *)kmalloc((size_t)nr_pages * sizeof(uint64_t));
    if (!frames)
        return -1;

    for (uint32_t i = 0; i < nr_pages; i++) {
        frames[i] = pmm_alloc_frame();
        if (!frames[i]) {
            for (uint32_t j = 0; j < i; j++)
                pmm_free_frame(frames[j]);
            kfree(frames);
            return -1;
        }
    }

    uint64_t data_phys = frames[0] * 4096;
    void *data_virt = PHYS_TO_VIRT((void*)(uintptr_t)data_phys);

    if (req->flags & BLK_REQ_WRITE) {
        /* For writes: copy data from request buffer to DMA buffer */
        memcpy(data_virt, req->buf, (size_t)nr_sectors * 512);
    }

    /* Build PRP list if multi-page (NVMe PRP entries cannot cross page boundaries) */
    uint64_t prp1 = data_phys;
    uint64_t prp2 = 0;
    uint64_t *prp_list = NULL;

    if (nr_pages > 1) {
        /* Single page boundary: if the transfer crosses 4KB within the first
         * page, the remaining data goes in PRP2 or a PRP list.  For simplicity,
         * use a PRP list when nr_pages > 1. */
        prp_list = (uint64_t *)PHYS_TO_VIRT(frames[1] * 4096); /* allocate PRP list in 2nd page */
        if (prp_list) {
            for (uint32_t i = 1; i < nr_pages; i++)
                prp_list[i - 1] = frames[i] * 4096;
            prp2 = frames[1] * 4096; /* PRP list physical address */
        }
    }

    /* Submit the I/O command with PRP1 and optional PRP list */
    int ret = nvme_io_submit_on_queue(q, nsid,
                                      !!(req->flags & BLK_REQ_WRITE),
                                      req->lba, nr_sectors, prp1, prp2, nr_pages);
    if (ret < 0) {
        for (uint32_t i = 0; i < nr_pages; i++)
            pmm_free_frame(frames[i]);
        kfree(frames);
        return -1;
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

    for (uint32_t i = 0; i < nr_pages; i++)
        pmm_free_frame(frames[i]);
    kfree(frames);
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
            kprintf("[NVMe] Failed to identify namespace %u\n", nsid);
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
            kprintf("[NVMe] Failed to register blockdev %s\n", devname);
            continue;
        }

        g_nvme_ctrl.ns_blkdev_id[ns_index] = dev_id;
        registered++;

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

        kprintf("[NVMe] Registered namespace %u: %s (%llu sectors of %u bytes)\n",
                nsid, devname, (unsigned long long)nsze, sector_size);
    }

    return registered;
}

/* ── Main initialization ───────────────────────────────────────────── */

int nvme_init(void) {
    if (g_nvme_init_done)
        return 0;

    memset(&g_nvme_ctrl, 0, sizeof(g_nvme_ctrl));

    /* Probe PCI */
    if (nvme_probe_pci() < 0) {
        kprintf("[NVMe] No NVMe controller found\n");
        g_nvme_init_done = 1;
        return -1;
    }

    /* Setup admin queues */
    if (nvme_setup_admin_queues() < 0) {
        kprintf("[NVMe] Failed to setup admin queues\n");
        return -1;
    }

    /* Enable controller */
    if (nvme_enable_controller() < 0) {
        kprintf("[NVMe] Failed to enable controller\n");
        return -1;
    }

    /* Identify controller */
    struct nvme_identify_ctrl id;
    if (nvme_identify_ctrl(&id) == 0) {
        kprintf("[NVMe] Model: %.40s  SN: %.20s\n", id.mn, id.sn);
        kprintf("[NVMe] FW rev: %.8s, Namespaces: %u\n", id.fr, id.nn);
        /* Save MDTS (Maximum Data Transfer Size) for bio splitting (Item 328) */
        g_nvme_ctrl.mdts = id.mdts;
    }

    /* Register IRQ handler */
    idt_register_handler(32 + g_nvme_ctrl.irq, nvme_irq_handler);

    /* Set up per-CPU I/O queue pairs */
    if (nvme_setup_io_queues() < 0) {
        kprintf("[NVMe] I/O queue setup failed\n");
    }

    /* Register namespaces as block devices */
    if (g_nvme_ctrl.nn > 0) {
        int reg = nvme_register_blockdevs();
        if (reg > 0)
            kprintf("[NVMe] Registered %d namespace(s)\n", reg);
        else
            kprintf("[NVMe] No namespaces registered\n");
    }

    g_nvme_init_done = 1;
    kprintf("[NVMe] Driver initialized\n");
    return 0;
}

/* ── Utility functions ─────────────────────────────────────────────── */

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
void nvme_exit(void)
{
    if (!g_nvme_ctrl.present || !g_nvme_init_done)
        return;

    kprintf("[NVMe] Shutting down...\n");

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

    kprintf("[NVMe] Driver shut down\n");
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
