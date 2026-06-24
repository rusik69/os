/* nvme_pmr.c — NVMe Persistent Memory Region (PMR) support (B9)
 *
 * Detects and exposes the NVMe PMR capability via PMRCAP register at
 * BAR0+0xE00.  Maps the PMR memory (typically BAR4) into kernel address
 * space and exposes it as a DAX-capable memory region.
 *
 * PMR Register Map (NVMe spec, offset from BAR0):
 *   PMRCAP  0xE00  — PMR Capabilities   [32-bit]
 *   PMRCTL  0xE04  — PMR Control         [32-bit]
 *   PMRSTS  0xE08  — PMR Status          [32-bit]
 *   PMREBS  0xE0C  — PMR Elasticity Buffer Size [32-bit]
 *   PMRSWTP 0xE10  — PMR Sustained Write Throughput [32-bit]
 *   PMRMSCL 0xE18  — PMR Misc Capabilities Low [32-bit]
 *   PMRMSCU 0E1C   — PMR Misc Capabilities High [32-bit]
 *   PMRMCAPL 0xE20 — PMR Memory Capability Low  [64-bit]
 *   PMRMCAPU 0xE28 — PMR Memory Capability High  [64-bit]
 *
 * PMR Memory BAR is typically BAR4 (or BAR5 for 32-bit).
 */

#include "nvme.h"
#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "heap.h"
#include "errno.h"
#include "blockdev.h"

/* ── Forward declarations ─────────────────────────────────────────── */
static int nvme_pmr_register_bdev(void);

/* ── PMR Register Offsets ────────────────────────────────────────── */
#define NVME_REG_PMRCAP   0xE00
#define NVME_REG_PMRCTL   0xE04
#define NVME_REG_PMRSTS   0xE08
#define NVME_REG_PMREBS   0xE0C
#define NVME_REG_PMRSWTP  0xE10
#define NVME_REG_PMRMSCL  0xE18
#define NVME_REG_PMRMSCU  0xE1C
#define NVME_REG_PMRMCAPL 0xE20
#define NVME_REG_PMRMCAPU 0xE28

/* PMRCAP bit definitions */
#define PMRCAP_PMR_SU      (1u << 0)   /* PMR Supported */
#define PMRCAP_PMR_RDS     (1u << 1)   /* PMR Region Data Stride */
#define PMRCAP_PMR_WDS     (1u << 2)   /* PMR Write Data Stride */
#define PMRCAP_PMR_BMASK   0x000000F8  /* Reserved */

/* PMRCTL bit definitions */
#define PMRCTL_PMR_EN      (1u << 0)   /* PMR Enable */
#define PMRCTL_CB_RW       (1u << 1)   /* Controller-Based Read/Write */

/* PMRSTS bit definitions */
#define PMRSTS_PMR_ERR     (1u << 0)   /* PMR Error */
#define PMRSTS_PMR_NRDY    (1u << 1)   /* PMR Not Ready */
#define PMRSTS_CBA         (1u << 2)   /* Controller-Based Abort */
#define PMRSTS_ESTBL       (1u << 3)   /* PMR HCI Establishent */

/* ── PMR state ───────────────────────────────────────────────────── */

struct nvme_pmr {
    int      present;       /* PMR capability detected */
    int      enabled;       /* PMR enabled on this controller */

    /* PMR memory BAR info */
    int      bar_index;     /* Which PCI BAR holds the PMR mapping */
    uint64_t pmr_phys;      /* Physical address of PMR memory window */
    uint64_t pmr_size;      /* Size of PMR memory window in bytes */
    void    *pmr_virt;      /* Kernel virtual address of PMR mapping */

    /* Capabilities */
    uint8_t  stride;        /* Region data stride (2^stride bytes) */
    uint64_t total_size;    /* Total PMR size in bytes */
    uint32_t elasticity_buf_size; /* Elasticity buffer size in 4KB units */
    uint32_t write_throughput;    /* Sustained write throughput in MB/s */

    /* Reference to parent controller regs */
    uint64_t ctrl_regs;     /* NVMe controller MMIO base (virtual) */
};

static struct nvme_pmr g_pmr;
static int g_pmr_init_done = 0;

/* ── MMIO helpers ────────────────────────────────────────────────── */

static inline uint32_t pmr_read32(uint64_t reg_offset) {
    return *(volatile uint32_t *)(uintptr_t)(g_pmr.ctrl_regs + reg_offset);
}

static inline void pmr_write32(uint64_t reg_offset, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(g_pmr.ctrl_regs + reg_offset) = val;
}

static inline uint64_t pmr_read64(uint64_t reg_offset) {
    return *(volatile uint64_t *)(uintptr_t)(g_pmr.ctrl_regs + reg_offset);
}

/* ── Probe PMR capabilities ──────────────────────────────────────── */

static int nvme_pmr_detect(uint64_t ctrl_regs, uint64_t ctrl_phys_regs) {
    g_pmr.ctrl_regs = ctrl_regs;

    /* Read PMRCAP to check for PMR support */
    uint32_t pmrcap = pmr_read32(NVME_REG_PMRCAP);
    if (!(pmrcap & PMRCAP_PMR_SU)) {
        kprintf("[NVMe PMR] PMR not supported by this controller (PMRCAP=0x%08X)\n", pmrcap);
        return -EOPNOTSUPP;
    }

    kprintf("[NVMe PMR] PMR capability detected (PMRCAP=0x%08X)\n", pmrcap);

    /* PMR Memory BAR must be found separately.  Per NVMe spec, if PMR_SU
     * is set, the controller should have the PMR memory mapped through a
     * separate PCI BAR (typically BAR4).  We probe for it. */
    g_pmr.present = 1;
    g_pmr.stride = (pmrcap & PMRCAP_PMR_RDS) ? 1 : 0;

    /* Read PMR Memory Capability (size) */
    uint64_t pmrcap_low  = pmr_read64(NVME_REG_PMRMCAPL);
    uint64_t pmrcap_high = pmr_read64(NVME_REG_PMRMCAPU);
    g_pmr.total_size = pmrcap_low;  /* lower 64 bits = size in bytes */

    kprintf("[NVMe PMR] Memory capability: total_size=%llu bytes (0x%llX)\n",
            (unsigned long long)g_pmr.total_size,
            (unsigned long long)g_pmr.total_size);

    /* Read elasticity buffer size (in 4KB units) */
    g_pmr.elasticity_buf_size = (uint32_t)((pmrcap_low >> 32) & 0xFFFFFFFF);

    /* Read PMR Sustained Write Throughput */
    g_pmr.write_throughput = pmr_read32(NVME_REG_PMRSWTP);

    return 0;
}

/* ── Find PMR memory BAR ─────────────────────────────────────────── */

static int nvme_pmr_find_bar(void) {
    /* PMR memory is typically exposed via BAR4 (or sometimes BAR5).
     * We search all PCI BARs of the NVMe controller for a memory region
     * that matches the PMR size from PMRMCAP. */

    /* Use PCI scanning to find the NVMe controller's BARs */
    struct pci_device pci;
    int ret = pci_find_class(NVME_PCI_CLASS, NVME_PCI_SUBCLASS, &pci);
    if (ret < 0) {
        struct pci_device pci2;
        uint32_t sub_prog = ((uint32_t)NVME_PCI_PROG_IF << 16) | NVME_PCI_SUBCLASS;
        ret = pci_find_class(NVME_PCI_CLASS, (uint8_t)sub_prog, &pci2);
        if (ret < 0)
            return -ENODEV;
        pci = pci2;
    }

    /* Look for a large memory BAR (BAR4 is typical for PMR) */
    for (int i = 4; i < 6; i++) {   /* BAR4 and BAR5 */
        uint32_t bar_val = pci.bar[i];
        if (bar_val == 0)
            continue;

        /* Check if this is an MMIO BAR (bit 0 = 0 for MMIO, = 1 for I/O) */
        if (bar_val & 1)
            continue;  /* I/O space, skip */

        /* Get the base address (high bits, low 4 bits = flags) */
        uint64_t base = (uint64_t)(bar_val & 0xFFFFFFF0);

        /* For 64-bit BARs, consume the next BAR as upper 32 bits */
        if ((bar_val & 0x6) == 0x4) {  /* 64-bit MMIO BAR */
            uint32_t bar_upper = pci.bar[i + 1];
            base |= ((uint64_t)bar_upper << 32);
        }

        /* PMR memory should be non-zero and typically quite large (MBs).
         * We don't know the exact size from the BAR itself without probing,
         * but we assume the large BAR >= 1 MB is PMR. */
        if (base && g_pmr.total_size >= 0x100000) {
            g_pmr.bar_index = i;
            g_pmr.pmr_phys = base;
            g_pmr.pmr_size = g_pmr.total_size;  /* from PMRMCAP */
            return 0;
        }
    }

    /* Fallback: try BAR0 if it's large enough (unlikely but possible) */
    if (g_pmr.total_size <= 0x100000) {
        for (int i = 0; i < 4; i++) {
            uint32_t bar_val = pci.bar[i];
            if (bar_val == 0 || (bar_val & 1)) continue;
            uint64_t base = (uint64_t)(bar_val & 0xFFFFFFF0);
            if ((bar_val & 0x6) == 0x4) {
                uint32_t bar_upper = pci.bar[i + 1];
                base |= ((uint64_t)bar_upper << 32);
            }
            if (base) {
                g_pmr.bar_index = i;
                g_pmr.pmr_phys = base;
                g_pmr.pmr_size = g_pmr.total_size;
                return 0;
            }
        }
    }

    kprintf("[NVMe PMR] Could not find PMR memory BAR\n");
    return -ENODEV;
}

/* ── Public API ──────────────────────────────────────────────────── */

int nvme_pmr_enable(void)
{
    if (!g_pmr.present)
        return -EOPNOTSUPP;
    if (g_pmr.enabled)
        return 0;

    /* Enable PMR in the controller */
    uint32_t pmrctl = pmr_read32(NVME_REG_PMRCTL);
    pmrctl |= PMRCTL_PMR_EN;
    pmr_write32(NVME_REG_PMRCTL, pmrctl);

    /* Check PMR status */
    uint32_t pmrsts = pmr_read32(NVME_REG_PMRSTS);
    if (pmrsts & PMRSTS_PMR_ERR) {
        kprintf("[NVMe PMR] Error enabling PMR (PMRSTS=0x%08X)\n", pmrsts);
        return -EIO;
    }

    if (pmrsts & PMRSTS_PMR_NRDY) {
        kprintf("[NVMe PMR] PMR not ready after enable (PMRSTS=0x%08X)\n", pmrsts);
        return -EAGAIN;
    }

    /* Map PMR memory into kernel address space */
    if (g_pmr.pmr_phys && !g_pmr.pmr_virt) {
        g_pmr.pmr_virt = PHYS_TO_VIRT((void*)(uintptr_t)g_pmr.pmr_phys);
    }

    g_pmr.enabled = 1;
    kprintf("[NVMe PMR] Enabled (BAR%d, PMR memory at %p, size=%llu bytes)\n",
            g_pmr.bar_index, g_pmr.pmr_virt,
            (unsigned long long)g_pmr.pmr_size);
    return 0;
}

int nvme_pmr_disable(void)
{
    if (!g_pmr.enabled)
        return 0;

    /* Disable PMR in the controller */
    uint32_t pmrctl = pmr_read32(NVME_REG_PMRCTL);
    pmrctl &= ~PMRCTL_PMR_EN;
    pmr_write32(NVME_REG_PMRCTL, pmrctl);

    g_pmr.enabled = 0;
    kprintf("[NVMe PMR] Disabled\n");
    return 0;
}

int __init nvme_pmr_init(void) {
    if (g_pmr_init_done)
        return g_pmr.present ? 0 : -EOPNOTSUPP;

    memset(&g_pmr, 0, sizeof(g_pmr));
    g_pmr.bar_index = -1;

    /* The NVMe controller should already be initialized.
     * We query the global nvme_ctrl for its register base. */
    /* Since nvme_ctrl is static in nvme.c, we use a probe approach:
     * find the PCI device and detect PMR on it. */
    struct pci_device pci;
    int ret = pci_find_class(NVME_PCI_CLASS, NVME_PCI_SUBCLASS, &pci);
    if (ret < 0) {
        struct pci_device pci2;
        uint32_t sub_prog = ((uint32_t)NVME_PCI_PROG_IF << 16) | NVME_PCI_SUBCLASS;
        ret = pci_find_class(NVME_PCI_CLASS, (uint8_t)sub_prog, &pci2);
        if (ret < 0) {
            kprintf("[NVMe PMR] No NVMe controller found\n");
            g_pmr_init_done = 1;
            return -ENODEV;
        }
        pci = pci2;
    }

    /* Get the NVMe register base (BAR0) */
    uint32_t bar0 = pci.bar[0];
    uint64_t mmio_base = (bar0 & 0xFFFFFFF0);
    if (mmio_base == 0) {
        kprintf("[NVMe PMR] BAR0 not found\n");
        g_pmr_init_done = 1;
        return -ENODEV;
    }
    uint64_t ctrl_regs_virt = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);

    /* Detect PMR capability via PMRCAP */
    ret = nvme_pmr_detect(ctrl_regs_virt, mmio_base);
    if (ret < 0) {
        g_pmr_init_done = 1;
        return ret;
    }

    /* Find the PMR memory BAR */
    ret = nvme_pmr_find_bar();
    if (ret < 0) {
        g_pmr_init_done = 1;
        return ret;
    }

    /* Enable PMR in the controller */
    uint32_t pmrctl = pmr_read32(NVME_REG_PMRCTL);
    pmrctl |= PMRCTL_PMR_EN;
    pmr_write32(NVME_REG_PMRCTL, pmrctl);

    /* Check PMR status */
    uint32_t pmrsts = pmr_read32(NVME_REG_PMRSTS);
    if (pmrsts & PMRSTS_PMR_ERR) {
        kprintf("[NVMe PMR] Error enabling PMR (PMRSTS=0x%08X)\n", pmrsts);
        g_pmr.present = 0;
        g_pmr_init_done = 1;
        return -EIO;
    }

    if (pmrsts & PMRSTS_PMR_NRDY) {
        kprintf("[NVMe PMR] PMR not ready after enable (PMRSTS=0x%08X)\n", pmrsts);
        g_pmr.present = 0;
        g_pmr_init_done = 1;
        return -EAGAIN;
    }

    /* Map PMR memory into kernel address space
     * The PMR is already accessible via the PCI BAR which is mapped by the
     * PCI subsystem during pci_find_class.  We map it if not already. */
    if (g_pmr.pmr_phys) {
        g_pmr.pmr_virt = PHYS_TO_VIRT((void*)(uintptr_t)g_pmr.pmr_phys);
        kprintf("[NVMe PMR] PMR memory mapped: phys=0x%llX virt=%p size=%llu bytes (%.2f MB)\n",
                (unsigned long long)g_pmr.pmr_phys,
                g_pmr.pmr_virt,
                (unsigned long long)g_pmr.pmr_size,
                (double)g_pmr.pmr_size / (1024.0 * 1024.0));
    }

    g_pmr.enabled = 1;
    g_pmr_init_done = 1;

    kprintf("[NVMe PMR] Initialized (B9): BAR%d, stride=%u, elasticity=%u KB, throughput=%u MB/s\n",
            g_pmr.bar_index,
            (unsigned)g_pmr.stride,
            (unsigned)(g_pmr.elasticity_buf_size * 4),
            (unsigned)g_pmr.write_throughput);

    /* Register as block device */
    nvme_pmr_register_bdev();

    return 0;
}

int nvme_pmr_read(uint64_t offset, void *buf, uint64_t len) {
    if (!g_pmr.enabled || !g_pmr.pmr_virt)
        return -EIO;

    if (offset + len > g_pmr.pmr_size)
        return -EINVAL;

    memcpy(buf, (uint8_t *)g_pmr.pmr_virt + offset, (size_t)len);
    return 0;
}

int nvme_pmr_write(uint64_t offset, const void *buf, uint64_t len) {
    if (!g_pmr.enabled || !g_pmr.pmr_virt)
        return -EIO;

    if (offset + len > g_pmr.pmr_size)
        return -EINVAL;

    memcpy((uint8_t *)g_pmr.pmr_virt + offset, buf, (size_t)len);

    /* Flush to persistent memory (if we have clwb/clflushopt)
     * For now, we rely on the platform's default cache policy.
     * A full implementation would use clwb for each cache line. */
    __sync_synchronize();

    return 0;
}

int nvme_pmr_is_present(void) {
    return g_pmr.present;
}

uint64_t nvme_pmr_get_size(void) {
    return g_pmr.present ? g_pmr.pmr_size : 0;
}

void *nvme_pmr_get_virt(void) {
    return g_pmr.present ? g_pmr.pmr_virt : NULL;
}

void nvme_pmr_exit(void) {
    if (!g_pmr.present)
        return;

    /* Unregister block device if registered */
    int dev_id = BLOCKDEV_PMEM0; /* Use PMEM0 slot for PMR */
    if (blockdev_is_registered(dev_id))
        blockdev_unregister(dev_id);

    /* Disable PMR */
    uint32_t pmrctl = pmr_read32(NVME_REG_PMRCTL);
    pmrctl &= ~PMRCTL_PMR_EN;
    pmr_write32(NVME_REG_PMRCTL, pmrctl);

    g_pmr.enabled = 0;
    g_pmr.present = 0;
    g_pmr.pmr_virt = NULL;
    g_pmr_init_done = 0;

    kprintf("[NVMe PMR] Shut down\n");
}

/* ── Block device driver ──────────────────────────────────────────── */

/*
 * nvme_pmr_submit — Block device submit callback for PMR.
 * Translates block I/O requests into direct memcpy to/from PMR memory.
 */
static int nvme_pmr_submit(struct blk_request *req)
{
    if (!req || !g_pmr.enabled || !g_pmr.pmr_virt)
        return -1;

    uint64_t lba = req->lba;
    uint32_t count = req->count;
    uint64_t offset = lba * 512ULL;
    uint64_t length = (uint64_t)count * 512ULL;

    if (offset + length > g_pmr.pmr_size) {
        req->result = -1;
        return -1;
    }

    void *pmr_addr = (uint8_t *)g_pmr.pmr_virt + offset;
    void *buf = req->buf;

    if (!buf) {
        req->result = -1;
        return -1;
    }

    if (req->flags & BLK_REQ_READ) {
        memcpy(buf, pmr_addr, (size_t)length);
    } else if (req->flags & BLK_REQ_WRITE) {
        memcpy(pmr_addr, buf, (size_t)length);
        __sync_synchronize();
    } else {
        req->result = -1;
        return -1;
    }

    req->result = 0;
    return 0;
}

/* ── Block device registration ────────────────────────────────────── */

static int g_pmr_bdev_registered = 0;

static int nvme_pmr_register_bdev(void)
{
    if (g_pmr_bdev_registered)
        return 0;
    if (!g_pmr.enabled || !g_pmr.pmr_virt)
        return -EIO;

    uint64_t sector_count = g_pmr.pmr_size / 512ULL;
    int dev_id = BLOCKDEV_PMEM0;

    int ret = blockdev_register(dev_id, "nvme_pmr",
                                 nvme_pmr_submit, NULL,
                                 sector_count, 0);
    if (ret != 0) {
        kprintf("[NVMe PMR] blockdev_register failed: %d\n", ret);
        return ret;
    }

    g_pmr_bdev_registered = 1;
    kprintf("[NVMe PMR] Registered as block device pmem%d (%llu sectors)\n",
            dev_id - BLOCKDEV_PMEM0,
            (unsigned long long)sector_count);
    return 0;
}

/* ── nvme_pmr_flush: flush PMR data to persistence ── */
int nvme_pmr_flush(void *dev)
{
    (void)dev;
    if (!g_pmr.enabled || !g_pmr.pmr_virt)
        return -EIO;

    /* For persistent memory, a cache flush ensures data reaches the
     * PM.  Use CLWB for each cache line followed by SFENCE. */
    uint64_t size = g_pmr.pmr_size;
    uintptr_t addr = (uintptr_t)g_pmr.pmr_virt;
    uintptr_t end = addr + size;

    for (; addr < end; addr += 64) {
        __asm__ volatile("clwb (%0)" : : "r"(addr) : "memory");
    }
    __asm__ volatile("sfence" : : : "memory");

    kprintf("[nvme] nvme_pmr_flush: %llu bytes flushed\n", (unsigned long long)size);
    return 0;
}

/* ── nvme_pmr_secure_erase: securely erase a PMR range ── */
int nvme_pmr_secure_erase(void *dev, uint64_t offset, size_t count)
{
    (void)dev;
    if (!g_pmr.enabled || !g_pmr.pmr_virt)
        return -EIO;

    if (offset + count > g_pmr.pmr_size)
        return -EINVAL;

    /* Overwrite with zeros, then flush */
    uint8_t *base = (uint8_t *)g_pmr.pmr_virt + offset;
    memset(base, 0, count);

    /* Flush to persistence */
    uintptr_t addr = (uintptr_t)base;
    uintptr_t end = addr + count;
    for (; addr < end; addr += 64) {
        __asm__ volatile("clwb (%0)" : : "r"(addr) : "memory");
    }
    __asm__ volatile("sfence" : : : "memory");

    kprintf("[nvme] nvme_pmr_secure_erase: offset=%llu count=%llu\n",
            (unsigned long long)offset, (unsigned long long)count);
    return 0;
}
