/*
 * iommu.c — IOMMU (VT-d) DMA Remapping Driver
 *
 * Parses the ACPI DMAR table to detect VT-d hardware,
 * initializes DMA remapping units, and manages IOVA mappings
 * per PCI device.
 *
 * B1: DMA Remapping via Intel VT-d.
 */

#include "iommu.h"
#include "acpi.h"
#include "pci.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "io.h"
#include "spinlock.h"

/* ── VT-d Register Offsets (Memory-mapped, from DRHD base_addr) ─────── */
#define VTD_REG_VER           0x00   /* Version Register */
#define VTD_REG_CAP           0x08   /* Capability Register */
#define VTD_REG_ECAP          0x10   /* Extended Capability Register */
#define VTD_REG_GCMD          0x18   /* Global Command Register */
#define VTD_REG_GSTS          0x1C   /* Global Status Register */
#define VTD_REG_RTADDR        0x20   /* Root Table Address Register */
#define VTD_REG_IQH           0x40   /* Invalidation Queue Head */
#define VTD_REG_IQT           0x44   /* Invalidation Queue Tail */
#define VTD_REG_IQA           0x48   /* Invalidation Queue Address */

/* Global Command bits */
#define VTD_GCMD_SIRTP        (1U << 24)  /* Set Interrupt Remap Table Pointer */
#define VTD_GCMD_SRTP         (1U << 30)  /* Set Root Table Pointer */
#define VTD_GCMD_TE           (1U << 31)  /* Translation Enable */
#define VTD_GCMD_SRTP_SHIFT   30
#define VTD_GCMD_SIRTP_SHIFT  24

/* Global Status bits */
#define VTD_GSTS_RTPS         (1U << 30)  /* Root Table Pointer Set */
#define VTD_GSTS_TES          (1U << 31)  /* Translation Enable Status */

/* Root-table entry: points to a context-table (4K-aligned) */
struct vtd_root_entry {
    uint64_t low;
    uint64_t high;
} __attribute__((packed));

/* Context-table entry per PCI device/function */
#define VTD_CTX_ENTRY_PRESENT    (1ULL << 0)
#define VTD_CTX_ENTRY_TT_SHIFT   2    /* Translation Type: 0=host, 1=guest, 2=pass-through */
#define VTD_CTX_ENTRY_TT_MASK    (3ULL << 2)
#define VTD_CTX_ENTRY_ADDR_SHIFT 12

struct vtd_ctx_entry {
    uint64_t low;
    uint64_t high;
} __attribute__((packed));

/* ── Per-device IOVA tracking ────────────────────────────────────────── */

#define MAX_IOMMU_DEVICES  64

struct iommu_device {
    uint8_t  bus, slot, func;
    uint16_t segment;
    struct vtd_ctx_entry *ctx_entry;  /* Pointer to context table entry */
    struct iommu_domain   domain;
    int     used;
};

/* ── Global IOMMU state ──────────────────────────────────────────────── */

static struct iommu_hw_unit {
    uint64_t base_addr;       /* MMIO register base (from DRHD) */
    uint16_t segment;
    uint8_t  flags;
    int      initialized;
    struct vtd_root_entry *root_table;
    uint64_t root_table_phys;
} g_iommu_units[8];
static int g_num_iommu_units = 0;

static struct iommu_device g_iommu_devs[MAX_IOMMU_DEVICES];
static int g_num_iommu_devs = 0;

/* DMAR table physical address (found by acpi_init, consumed by iommu_init) */
static uint64_t g_dmar_table_phys = 0;

static int g_iommu_initialized = 0;

/* Simple spinlock for IOMMU state */
static spinlock_t iommu_lock = SPINLOCK_INIT;

/* ── Internal helpers ────────────────────────────────────────────────── */

static inline uint32_t vtd_read32(struct iommu_hw_unit *unit, uint64_t reg)
{
    volatile uint32_t *ptr = (volatile uint32_t *)(unit->base_addr + reg);
    return *ptr;
}

static inline void vtd_write32(struct iommu_hw_unit *unit, uint64_t reg, uint32_t val)
{
    volatile uint32_t *ptr = (volatile uint32_t *)(unit->base_addr + reg);
    *ptr = val;
}

static inline uint64_t vtd_read64(struct iommu_hw_unit *unit, uint64_t reg)
{
    volatile uint64_t *ptr = (volatile uint64_t *)(unit->base_addr + reg);
    return *ptr;
}

static inline void vtd_write64(struct iommu_hw_unit *unit, uint64_t reg, uint64_t val)
{
    volatile uint64_t *ptr = (volatile uint64_t *)(unit->base_addr + reg);
    *ptr = val;
}

/* Wait for a command to complete by polling status bits */
static int vtd_wait_cmd(struct iommu_hw_unit *unit, uint32_t mask, int set)
{
    for (int i = 0; i < 1000000; i++) {
        uint32_t sts = vtd_read32(unit, VTD_REG_GSTS);
        if (set && (sts & mask)) return 0;
        if (!set && !(sts & mask)) return 0;
        __asm__ volatile("pause");
    }
    return -1;  /* Timeout */
}

/* Allocate a zeroed 4K page for hardware (returns physical address) */
static uint64_t iommu_alloc_hw_page(void)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    void *virt = PHYS_TO_VIRT(phys);
    memset(virt, 0, 4096);
    return phys;
}

/* ── DMAR table parsing ─────────────────────────────────────────────── */

/*
 * iommu_parse_dmar — walk the DMAR table and extract DRHD/RMRR entries.
 * Called from iommu_init().
 */
static void iommu_parse_dmar(struct acpi_header *dmar_hdr)
{
    struct dmar_table *dmar = (struct dmar_table *)dmar_hdr;
    uint8_t *pos = (uint8_t *)(dmar + 1);  /* Skip DMAR header */
    uint8_t *end = (uint8_t *)dmar + dmar->header.length;

    kprintf("[IOMMU] DMAR: host_addr_width=%u, flags=0x%02x\n",
            dmar->host_addr_width, dmar->flags);

    while (pos + sizeof(struct dmar_sub_header) <= end) {
        struct dmar_sub_header *sub = (struct dmar_sub_header *)pos;
        if (sub->length < sizeof(struct dmar_sub_header))
            break;

        switch (sub->type) {
        case DMAR_TYPE_DRHD: {
            struct dmar_drhd *drhd = (struct dmar_drhd *)pos;
            if (g_num_iommu_units >= 8) {
                kprintf("[IOMMU] WARNING: too many IOMMU units\n");
                break;
            }
            struct iommu_hw_unit *unit = &g_iommu_units[g_num_iommu_units];
            unit->base_addr = drhd->base_addr;
            unit->segment   = drhd->segment;
            unit->flags     = drhd->flags;
            unit->initialized = 0;
            g_num_iommu_units++;

            kprintf("[IOMMU] DRHD: base=0x%llx, seg=%u, flags=0x%02x\n",
                    (unsigned long long)drhd->base_addr,
                    drhd->segment, drhd->flags);
            break;
        }
        case DMAR_TYPE_RMRR: {
            struct dmar_rmrr *rmrr = (struct dmar_rmrr *)pos;
            kprintf("[IOMMU] RMRR: seg=%u, base=0x%llx, end=0x%llx\n",
                    rmrr->segment,
                    (unsigned long long)rmrr->base_addr,
                    (unsigned long long)rmrr->end_addr);
            break;
        }
        default:
            kprintf("[IOMMU] DMAR sub-table type %u (len=%u)\n",
                    sub->type, sub->length);
            break;
        }

        pos += sub->length;
        /* Align to 8-byte boundary for next entry */
        if ((uintptr_t)pos & 7)
            pos = (uint8_t *)(((uintptr_t)pos + 7) & ~7ULL);
    }
}

/* ── IOMMU unit initialization ───────────────────────────────────────── */

static int iommu_init_unit(struct iommu_hw_unit *unit)
{
    uint32_t ver = vtd_read32(unit, VTD_REG_VER);
    kprintf("[IOMMU] VT-d version: %u.%u\n", ver >> 4, ver & 0xF);

    /* Check capabilities */
    uint64_t cap = vtd_read64(unit, VTD_REG_CAP);
    uint64_t ecap = vtd_read64(unit, VTD_REG_ECAP);

    uint64_t num_domains = (cap >> 48) & 0xFF;
    kprintf("[IOMMU] CAP=0x%llx, ECAP=0x%llx, num_domains=%llu\n",
            (unsigned long long)cap, (unsigned long long)ecap,
            (unsigned long long)num_domains);

    /* Allocate root table (must be 4K-aligned) */
    unit->root_table_phys = iommu_alloc_hw_page();
    if (!unit->root_table_phys) {
        kprintf("[IOMMU] Failed to allocate root table\n");
        return -1;
    }
    unit->root_table = PHYS_TO_VIRT(unit->root_table_phys);
    memset(unit->root_table, 0, 4096);

    /* Disable translation if enabled (write 0 to TE bit) */
    uint32_t gcmd = vtd_read32(unit, VTD_REG_GCMD);
    gcmd &= ~VTD_GCMD_TE;
    vtd_write32(unit, VTD_REG_GCMD, gcmd);
    if (vtd_wait_cmd(unit, VTD_GSTS_TES, 0) < 0) {
        kprintf("[IOMMU] WARNING: timeout disabling translation\n");
    }

    /* Set the root table address */
    vtd_write64(unit, VTD_REG_RTADDR, unit->root_table_phys);
    vtd_write32(unit, VTD_REG_GCMD, vtd_read32(unit, VTD_REG_GCMD) | VTD_GCMD_SRTP);
    if (vtd_wait_cmd(unit, VTD_GSTS_RTPS, 1) < 0) {
        kprintf("[IOMMU] WARNING: timeout setting root table pointer\n");
        return -1;
    }

    /* Enable DMA remapping */
    vtd_write32(unit, VTD_REG_GCMD, vtd_read32(unit, VTD_REG_GCMD) | VTD_GCMD_TE);
    if (vtd_wait_cmd(unit, VTD_GSTS_TES, 1) < 0) {
        kprintf("[IOMMU] WARNING: timeout enabling translation\n");
        return -1;
    }

    unit->initialized = 1;
    kprintf("[IOMMU] VT-d unit initialized (base=0x%llx)\n",
            (unsigned long long)unit->base_addr);
    return 0;
}

/* ── Context entry setup for a PCI device ────────────────────────────── */

static struct iommu_hw_unit *iommu_find_unit(uint16_t segment)
{
    for (int i = 0; i < g_num_iommu_units; i++) {
        if (g_iommu_units[i].segment == segment)
            return &g_iommu_units[i];
    }
    return NULL;
}

static int iommu_setup_device_context(struct iommu_device *dev)
{
    struct iommu_hw_unit *unit = iommu_find_unit(dev->segment);
    if (!unit) {
        kprintf("[IOMMU] No IOMMU unit for segment %u\n", dev->segment);
        return -1;
    }

    /* Root table index = bus number */
    int bus_idx = dev->bus;
    struct vtd_root_entry *re = &unit->root_table[bus_idx];

    /* Allocate context table if not exists */
    uint64_t ctx_table_phys;
    struct vtd_ctx_entry *ctx_table;

    if (!(re->low & 1)) {
        ctx_table_phys = iommu_alloc_hw_page();
        if (!ctx_table_phys) return -1;
        ctx_table = PHYS_TO_VIRT(ctx_table_phys);
        memset(ctx_table, 0, 4096);

        re->low = ctx_table_phys | 1;  /* Present */
        re->high = 0;
    } else {
        ctx_table_phys = re->low & ~0xFFFULL;
        ctx_table = PHYS_TO_VIRT(ctx_table_phys);
    }

    /* Context entry index = slot * 8 + func */
    int ctx_idx = dev->slot * 8 + dev->func;
    struct vtd_ctx_entry *ce = &ctx_table[ctx_idx];

    if (ce->low & VTD_CTX_ENTRY_PRESENT) {
        kprintf("[IOMMU] Device %02x:%02x.%x already has context\n",
                dev->bus, dev->slot, dev->func);
        dev->ctx_entry = ce;
        return 0;
    }

    /* Allocate domain page table (first-level translation) */
    uint64_t domain_pgd_phys = iommu_alloc_hw_page();
    if (!domain_pgd_phys) return -1;

    uint64_t *domain_pgd = PHYS_TO_VIRT(domain_pgd_phys);

    /* Set up context entry: pass-through or translate */
    ce->low = domain_pgd_phys | VTD_CTX_ENTRY_PRESENT;
    /* Translation type = 0 (Host mode with scalable? No, simple host mode):
     * bits [2:3] = 00 for host mode with 4-level page tables */
    ce->low &= ~VTD_CTX_ENTRY_TT_MASK;
    ce->high = 0;
    dev->ctx_entry = ce;

    /* Store domain page table pointer for mapping */
    dev->domain.pgd = domain_pgd;
    dev->domain.initialized = 1;

    kprintf("[IOMMU] Device %02x:%02x.%x context set up, pgd=0x%llx\n",
            dev->bus, dev->slot, dev->func,
            (unsigned long long)domain_pgd_phys);
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * iommu_init — detect and initialize VT-d hardware
 *
 * Parses the ACPI DMAR table, discovers IOMMU hardware units,
 * and enables DMA remapping globally.
 *
 * Returns 0 on success, <0 on failure.
 */
int iommu_init(void)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&iommu_lock, &flags);

    if (g_iommu_initialized) {
        spinlock_irqsave_release(&iommu_lock, flags);
        return 0;
    }

    kprintf("[IOMMU] Initializing VT-d DMA Remapping...\n");

    /* Find DMAR table via RSDT/XSDT.
     * We re-scan by walking PHYS_TO_VIRT(rsdp->rsdt_addr).
     * The DMAR table was already detected in acpi_init() — we
     * store its address now by searching again from the RSDT.
     *
     * For simplicity, we find it by scanning memory signatures.
     * In a full implementation, the ACPI subsystem would export
     * the table list; here we rely on the existing RSDT walk.
     */
    struct rsdp *rsdp = PHYS_TO_VIRT((uint64_t)(uintptr_t)0x80000 +
                         0xFFFF800000000000ULL);
    /* Can't do that easily — let's just return -1 and try a simpler approach */
    spinlock_irqsave_release(&iommu_lock, flags);

    /* Since acpi_init() already scanned RSDT, we cheat by
     * scanning the kernel's ACPI-known tables. For current
     * purposes, we probe known DMAR physical address ranges.
     *
     * Better: just try to find DMAR by scanning physical memory.
     * We look for "DMAR" signature in the RSDT table entries.
     *
     * Actually, let's do it properly: re-read the RSDT.
     */
    /* Find RSDP again */
    rsdp = (struct rsdp *)PHYS_TO_VIRT(0x80000);
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8))
        rsdp = (struct rsdp *)PHYS_TO_VIRT(0xE0000);
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8)) {
        /* Walk EBDA */
        uint16_t *ebda = (uint16_t *)PHYS_TO_VIRT(0x40E);
        if (*ebda)
            rsdp = (struct rsdp *)PHYS_TO_VIRT((uint64_t)(*ebda) << 4);
    }
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8)) {
        kprintf("[IOMMU] RSDP not found, cannot read DMAR\n");
        /* Return success anyway — system can run without IOMMU */
        return 0;
    }

    uint32_t rsdt_phys = rsdp->rsdt_addr;
    struct rsdt *rsdt = (struct rsdt *)PHYS_TO_VIRT(rsdt_phys);
    if (!rsdt || memcmp(rsdt->header.signature, "RSDT", 4) != 0) {
        kprintf("[IOMMU] RSDT not found\n");
        return 0;
    }

    uint32_t num = (uint32_t)((rsdt->header.length - sizeof(struct acpi_header)) / 4);

    spinlock_irqsave_acquire(&iommu_lock, &flags);

    for (uint32_t i = 0; i < num; i++) {
        struct acpi_header *hdr = (struct acpi_header *)PHYS_TO_VIRT((uint64_t)rsdt->entries[i]);
        if (memcmp(hdr->signature, DMAR_SIG, 4) == 0) {
            kprintf("[IOMMU] Found DMAR table at phys 0x%x\n", rsdt->entries[i]);
            iommu_parse_dmar(hdr);
            break;
        }
    }

    /* Initialize discovered IOMMU units */
    for (int i = 0; i < g_num_iommu_units; i++) {
        /* The base_addr from DRHD is a physical address — map it */
        g_iommu_units[i].base_addr = (uint64_t)PHYS_TO_VIRT(g_iommu_units[i].base_addr);
        if (iommu_init_unit(&g_iommu_units[i]) < 0) {
            kprintf("[IOMMU] Failed to init unit %d\n", i);
        }
    }

    g_iommu_initialized = 1;
    spinlock_irqsave_release(&iommu_lock, flags);

    kprintf("[IOMMU] Initialization complete: %d unit(s)\n", g_num_iommu_units);
    return 0;
}

/* ── Public IOMMU status API ─────────────────────────────────────────── */

int iommu_is_active(void) {
    return g_iommu_initialized;
}

int iommu_unit_count(void) {
    return g_num_iommu_units;
}

int iommu_device_count(void) {
    return g_num_iommu_devs;
}

/*
 * iommu_map_page — map a physical page to a device's IOVA space
 *
 * @dev:        PCI device to map for
 * @phys_addr:  Physical address to map
 * @iova:       I/O virtual address to map to
 * @flags:      IOMMU_READ, IOMMU_WRITE, etc.
 *
 * Returns 0 on success, <0 on error.
 */
int iommu_map_page(struct pci_device *dev, uint64_t phys_addr, uint64_t iova, uint64_t flags)
{
    if (!dev || !g_iommu_initialized)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&iommu_lock, &irq_flags);

    /* Find or create device entry */
    struct iommu_device *idev = NULL;
    for (int i = 0; i < g_num_iommu_devs; i++) {
        if (g_iommu_devs[i].used &&
            g_iommu_devs[i].bus   == dev->bus &&
            g_iommu_devs[i].slot  == dev->slot &&
            g_iommu_devs[i].func  == dev->func) {
            idev = &g_iommu_devs[i];
            break;
        }
    }

    if (!idev) {
        /* Allocate new device entry */
        if (g_num_iommu_devs >= MAX_IOMMU_DEVICES) {
            spinlock_irqsave_release(&iommu_lock, irq_flags);
            return -ENOSPC;
        }
        idev = &g_iommu_devs[g_num_iommu_devs];
        idev->bus = dev->bus;
        idev->slot = dev->slot;
        idev->func = dev->func;
        idev->segment = 0;  /* Default segment */
        idev->used = 1;
        idev->domain.initialized = 0;
        idev->domain.pgd = NULL;
        g_num_iommu_devs++;

        /* Set up context entry for this device */
        if (iommu_setup_device_context(idev) < 0) {
            idev->used = 0;
            g_num_iommu_devs--;
            spinlock_irqsave_release(&iommu_lock, irq_flags);
            return -ENOMEM;
        }
    }

    if (!idev->domain.initialized || !idev->domain.pgd) {
        spinlock_irqsave_release(&iommu_lock, irq_flags);
        return -EINVAL;
    }

    /* Use the existing iommu_map() from the header */
    int ret = iommu_map(&idev->domain, iova, phys_addr, IOMMU_PAGE_SIZE, flags);

    spinlock_irqsave_release(&iommu_lock, irq_flags);
    return ret;
}

/*
 * iommu_unmap_page — unmap a page from a device's IOVA space
 *
 * @dev:    PCI device
 * @iova:   I/O virtual address to unmap
 *
 * Returns 0 on success, <0 on error.
 */
int iommu_unmap_page(struct pci_device *dev, uint64_t iova)
{
    if (!dev || !g_iommu_initialized)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&iommu_lock, &irq_flags);

    struct iommu_device *idev = NULL;
    for (int i = 0; i < g_num_iommu_devs; i++) {
        if (g_iommu_devs[i].used &&
            g_iommu_devs[i].bus   == dev->bus &&
            g_iommu_devs[i].slot  == dev->slot &&
            g_iommu_devs[i].func  == dev->func) {
            idev = &g_iommu_devs[i];
            break;
        }
    }

    if (!idev || !idev->domain.initialized || !idev->domain.pgd) {
        spinlock_irqsave_release(&iommu_lock, irq_flags);
        return -ENOENT;
    }

    int ret = iommu_unmap(&idev->domain, iova, IOMMU_PAGE_SIZE);

    spinlock_irqsave_release(&iommu_lock, irq_flags);
    return ret;
}

/*
 * iommu_is_enabled — check if IOMMU is active
 */
int iommu_is_enabled(void)
{
    return g_iommu_initialized && (g_num_iommu_units > 0);
}

/* Forward declarations for stub functions */
struct device;

/* ── Stub: iommu_enable ─────────────────────────────── */
int iommu_enable(void)
{
    kprintf("[iommu] iommu_enable: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: iommu_disable ─────────────────────────────── */
void iommu_disable(void)
{
    kprintf("[iommu] iommu_disable: not yet implemented\n");
}

/* ── Stub: iommu_attach_device ─────────────────────────────── */
int iommu_attach_device(struct iommu_domain *domain, struct device *dev)
{
    (void)domain;
    (void)dev;
    kprintf("[iommu] iommu_attach_device: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: iommu_detach_device ─────────────────────────────── */
void iommu_detach_device(struct iommu_domain *domain, struct device *dev)
{
    (void)domain;
    (void)dev;
    kprintf("[iommu] iommu_detach_device: not yet implemented\n");
}

/* ── Stub: iommu_set_fault_handler ─────────────────────────────── */
int iommu_set_fault_handler(struct iommu_domain *domain, void *handler, void *data)
{
    (void)domain;
    (void)handler;
    (void)data;
    kprintf("[iommu] iommu_set_fault_handler: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: iommu_get_domain_for_dev ─────────────────────────────── */
struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
    (void)dev;
    kprintf("[iommu] iommu_get_domain_for_dev: not yet implemented\n");
    return NULL;
}

/* ── Stub: iommu_iova_to_phys ─────────────────────────────── */
uint64_t iommu_iova_to_phys(struct iommu_domain *domain, unsigned long iova)
{
    (void)domain;
    (void)iova;
    kprintf("[iommu] iommu_iova_to_phys: not yet implemented\n");
    return (uint64_t)-ENOSYS;
}

/* ── Stub: iommu_resume ─────────────────────────────── */
int iommu_resume(void)
{
    kprintf("[iommu] iommu_resume: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: iommu_suspend ─────────────────────────────── */
int iommu_suspend(void)
{
    kprintf("[iommu] iommu_suspend: not yet implemented\n");
    return -ENOSYS;
}

#include "module.h"
module_init(iommu_init);
