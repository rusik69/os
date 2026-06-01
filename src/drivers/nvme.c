/*
 * nvme.c — NVMe (NVM Express) PCI driver
 *
 * Proper PCI NVMe controller driver with admin queue setup.
 * Probes for NVMe devices via PCI class code (0x01, 0x08, 0x02).
 */

#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"

static struct nvme_ctrl g_nvme_ctrl;
static int g_nvme_init_done = 0;

/* Read from MMIO register */
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

/* Wait for CSTS.RDY bit to match expected state */
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

/* Probe for an NVMe device via PCI */
static int nvme_probe_pci(void) {
    struct pci_device pci;
    int ret = pci_find_class(NVME_PCI_CLASS, NVME_PCI_SUBCLASS, &pci);
    if (ret < 0) {
        /* Try with prog_if filter */
        struct pci_device pci2;
        int ret2 = pci_find_class(NVME_PCI_CLASS, NVME_PCI_SUBCLASS | (NVME_PCI_PROG_IF << 16), &pci2);
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

    kprintf("[NVMe] Found controller: VID=0x%04X DID=0x%04X IRQ=%d\n",
            pci.vendor_id, pci.device_id, pci.irq);
    kprintf("[NVMe] Version %d.%d.%d, max queue depth %u\n",
            (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF,
            g_nvme_ctrl.max_q_depth);

    g_nvme_ctrl.present = 1;
    return 0;
}

/* Initialize admin submission and completion queues */
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

    /* Set admin SQ base address (ASQ) */
    nvme_write64(&g_nvme_ctrl, NVME_REG_ASQ, g_nvme_ctrl.admin_sq_phys);
    nvme_write64(&g_nvme_ctrl, NVME_REG_ACQ, g_nvme_ctrl.admin_cq_phys);

    return 0;
}

static int nvme_enable_controller(void) {
    int ret;

    /* Wait for CSTS.RDY = 0 */
    ret = nvme_wait_ready(&g_nvme_ctrl, 0, 2000);
    if (ret < 0) {
        kprintf("[NVMe] Timeout waiting for CSTS.RDY=0\n");
        return -1;
    }

    /* Set CC: enable, page size (4096 = 0 shift), MPS=0, CSS=NVM, SQES=6, CQES=4 */
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

int nvme_submit_admin_cmd(struct nvme_sq_entry *cmd, struct nvme_cq_entry *cqe) {
    if (!g_nvme_ctrl.present || !g_nvme_ctrl.admin_sq)
        return -1;

    struct nvme_sq_entry *sq = (struct nvme_sq_entry *)g_nvme_ctrl.admin_sq;
    uint16_t tail = g_nvme_ctrl.admin_sq_tail;

    /* Copy command to submission queue */
    memcpy(&sq[tail], cmd, sizeof(struct nvme_sq_entry));
    tail = (tail + 1) % 64;
    g_nvme_ctrl.admin_sq_tail = tail;

    /* Ring the admin doorbell (doorbell stride from CAP.DSTRD) */
    uint64_t cap = nvme_read64(&g_nvme_ctrl, NVME_REG_CAP);
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xF);
    uint32_t doorbell_offset = 0x1000 + (dstrd * 4); /* Admin SQ doorbell */
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
            return 0;
        }
        __asm__ volatile("pause");
    }

    return -1;
}

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
        kprintf("[NVMe] Identify command failed\n");
    }

    pmm_free_frame(data_frame);
    return ret;
}

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
    }

    g_nvme_init_done = 1;
    kprintf("[NVMe] Driver initialized\n");
    return 0;
}

int nvme_is_present(void) {
    return g_nvme_ctrl.present;
}

void nvme_print_info(void) {
    if (!g_nvme_ctrl.present) {
        kprintf("NVMe: Not present\n");
        return;
    }
    kprintf("NVMe: present, regs at 0x%llX, max_q_depth=%u, namespaces=%u\n",
            (uint64_t)g_nvme_ctrl.phys_regs,
            g_nvme_ctrl.max_q_depth,
            g_nvme_ctrl.nn);
}
