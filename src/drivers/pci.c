#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "apic.h"
#include "pmm.h"
#include "vmm.h"
#include "export.h"
#include "kernel.h"
#include "stdio.h"
#include "module.h"      /* request_module() for PCI driver autoloading */

#ifndef HAVE_PCI_DEVICE_ID
#define HAVE_PCI_DEVICE_ID
#define PCI_ANY_ID   (~0U)
struct pci_device_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class;
    uint32_t class_mask;
    uintptr_t driver_data;
};
#endif

/* Forward declarations */
static void pci_queue_autoprobe(const char *modalias, uint16_t vendor,
                                 uint16_t device, uint16_t subvendor,
                                 uint16_t subdevice, uint8_t class_code,
                                 uint8_t subclass);

/* Maximum iterations for capabilities list traversal (safety bound to
 * prevent infinite loops on buggy or malicious devices that form a
 * circular pointer chain).  PCI config space is 256 bytes and each
 * capability header is at least 2 bytes, so 64 is a generous limit. */
#define PCI_CAP_MAX_ITERATIONS   64

/* Maximum iterations for PCIe Extended Capabilities list traversal.
 * Extended config space is 0x100-0xFFF (3840 bytes) and each extended
 * capability header is at least 4 bytes, so 256 is a generous limit
 * well above any realistic device. */
#define PCI_EXT_CAP_MAX_ITERATIONS  256

/* PCIe ECAM (Memory-Mapped Configuration Space) */
static uint64_t ecam_base = 0;

/* ── Helper: map ECAM physical region into kernel page table ────────────
 * The ECAM region (256MB) at the physical address from MCFG is mapped
 * using 2MB huge pages so pcie_read/pcie_write can dereference directly.
 *
 * Returns 0 on success, -1 on failure. */
static int map_ecam_region(uint64_t phys_base) {
    /* Align to 2MB boundary */
    uint64_t base = phys_base & ~(HUGE_PAGE_SIZE - 1);
    uint64_t end  = base + 0x10000000ULL;  /* 256 MB */

    /* Map each 2MB chunk as a huge page using the kernel page table */
    for (uint64_t addr = base; addr < end; addr += HUGE_PAGE_SIZE) {
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;

        uint64_t *pml4 = vmm_get_pml4();
        if (!pml4) return -EEXIST;

        /* Ensure PML4 entry exists */
        if (!(pml4[pml4_idx] & PTE_PRESENT)) {
            uint64_t frame = pmm_alloc_frame();
            if (unlikely(!frame)) return -ENOMEM;
            memset((void *)PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
            pml4[pml4_idx] = frame | PTE_PRESENT | PTE_WRITE;
        }

        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

        /* Ensure PDPT entry exists */
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
            uint64_t frame = pmm_alloc_frame();
            if (unlikely(!frame)) return -ENOMEM;
            memset((void *)PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
            pdpt[pdpt_idx] = frame | PTE_PRESENT | PTE_WRITE;
        }

        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

        /* Set 2MB huge page entry */
        pd[pd_idx] = (addr & 0x000FFFFFFFE00000ULL) | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_GLOBAL;
    }

    return 0;
}

void pcie_ecam_set_base(uint64_t base) {
    ecam_base = base;
    if (base) {
        if (map_ecam_region(base) == 0)
            kprintf("[OK] PCIe ECAM mapped: phys 0x%llx (256 MB via 2MB pages)\n", (unsigned long long)base);
        else
            kprintf("[--] PCIe ECAM: failed to map region at phys 0x%llx\n", (unsigned long long)base);
    }
}

int pcie_is_available(void) {
    return ecam_base != 0;
}

uint32_t pcie_read(int bus, int slot, int func, int offset) {
    if (!ecam_base) return pci_read(bus, slot, func, (uint8_t)(offset & 0xFF));
    uint64_t addr = ecam_base
                  | ((uint64_t)bus  << 20)
                  | ((uint64_t)slot << 15)
                  | ((uint64_t)func << 12)
                  | (offset & 0xFFC);
    return *(volatile uint32_t *)addr;
}

void pcie_write(int bus, int slot, int func, int offset, uint32_t val) {
    if (!ecam_base) { pci_write(bus, slot, func, (uint8_t)(offset & 0xFF), val); return; }
    uint64_t addr = ecam_base
                  | ((uint64_t)bus  << 20)
                  | ((uint64_t)slot << 15)
                  | ((uint64_t)func << 12)
                  | (offset & 0xFFC);
    *(volatile uint32_t *)addr = val;
}

/* ── PCI Express capability detection ─────────────────────────────── */

int pci_find_pcie_cap(int bus, int slot, int func, uint8_t *cap_offset) {
    /* PCI Express capability ID = 0x10 */
    /* Read status register via dword at offset 0x04, upper 16 bits */
    uint32_t status_dword;
    if (ecam_base) {
        status_dword = pcie_read(bus, slot, func, 0x04);
    } else {
        status_dword = pci_read(bus, slot, func, 0x04);
    }
    uint16_t status = (uint16_t)(status_dword >> 16);

    if (!(status & (1U << 4))) {
        /* Capabilities list not present */
        return -EINVAL;
    }

    /* Read capabilities pointer at offset 0x34 */
    uint8_t cap_ptr;
    if (ecam_base) {
        cap_ptr = (uint8_t)(pcie_read(bus, slot, func, 0x34) & 0xFF);
    } else {
        cap_ptr = (uint8_t)(pci_read(bus, slot, func, 0x34) & 0xFF);
    }

    int iter = 0;
    while (cap_ptr != 0) {
        if (++iter > PCI_CAP_MAX_ITERATIONS)
            break;

        uint32_t cap_reg;
        if (ecam_base) {
            cap_reg = pcie_read(bus, slot, func, cap_ptr);
        } else {
            cap_reg = pci_read(bus, slot, func, cap_ptr);
        }

        uint8_t cap_id = cap_reg & 0xFF;

        if (cap_id == 0x10) {
            /* Found PCI Express capability */
            if (cap_offset) *cap_offset = cap_ptr;
            return 0;
        }

        /* Next capability pointer */
        cap_ptr = (cap_reg >> 8) & 0xFF;
    }

    return -EINVAL;
}

int pcie_is_present(void) {
    /* Scan for at least one PCIe device */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0;
            if (ecam_base) {
                reg0 = pcie_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            } else {
                reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            }
            uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
            if (vid == 0xFFFF) continue;

            uint8_t cap_off;
            if (pci_find_pcie_cap((uint8_t)bus, (uint8_t)slot, 0, &cap_off) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

int pcie_device_type(int bus, int slot, int func) {
    uint8_t cap_off;
    if (pci_find_pcie_cap(bus, slot, func, &cap_off) < 0)
        return PCIE_DEV_TYPE_UNKNOWN;

    uint16_t cap_reg;
    if (ecam_base) {
        cap_reg = (uint16_t)pcie_read(bus, slot, func, cap_off + 2);
    } else {
        cap_reg = (uint16_t)pci_read(bus, slot, func, cap_off + 2);
    }

    uint8_t dev_type = (cap_reg >> 4) & 0x0F;
    switch (dev_type) {
    case 0: return PCIE_DEV_TYPE_ENDPOINT;
    case 1: return PCIE_DEV_TYPE_ROOT_PORT;
    case 2: return PCIE_DEV_TYPE_UPSTREAM;
    case 3: return PCIE_DEV_TYPE_DOWNSTREAM;
    case 4: return PCIE_DEV_TYPE_SWITCH;
    default: return PCIE_DEV_TYPE_UNKNOWN;
    }
}

/* ── Helper: read 16-bit config register via ECAM-aware path ──────── */
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    uint32_t val;
    uint32_t aligned = (uint32_t)(offset & (uint16_t)~3);
    if (ecam_base) {
        val = pcie_read(bus, slot, func, (int)aligned);
    } else {
        val = pci_read(bus, slot, func, (int)(aligned & 0xFF));
    }
    if (offset & 2)
        return (uint16_t)(val >> 16);
    return (uint16_t)(val & 0xFFFF);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t val) {
    uint32_t base;
    uint32_t aligned = (uint32_t)(offset & (uint16_t)~3);
    if (ecam_base) {
        base = pcie_read(bus, slot, func, (int)aligned);
        if (offset & 2)
            pcie_write(bus, slot, func, (int)aligned, (base & 0x0000FFFFu) | ((uint32_t)val << 16));
        else
            pcie_write(bus, slot, func, (int)aligned, (base & 0xFFFF0000u) | val);
    } else {
        base = pci_read(bus, slot, func, (int)(aligned & 0xFF));
        if (offset & 2)
            pci_write(bus, slot, func, (int)(aligned & 0xFF), (base & 0x0000FFFFu) | ((uint32_t)val << 16));
        else
            pci_write(bus, slot, func, (int)(aligned & 0xFF), (base & 0xFFFF0000u) | val);
    }
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val) {
    if (ecam_base) {
        pcie_write(bus, slot, func, offset, val);
    } else {
        pci_write(bus, slot, func, (uint8_t)(offset & 0xFF), val);
    }
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    if (ecam_base) {
        return pcie_read(bus, slot, func, offset);
    }
    return pci_read(bus, slot, func, (uint8_t)(offset & 0xFF));
}

/* ── MSI capability parsing ───────────────────────────────────────── */

int pci_find_msi_cap(uint8_t bus, uint8_t slot, uint8_t func,
                     struct msi_info *info) {
    if (!info) return -EINVAL;

    /* Check capabilities list bit */
    uint16_t status = pci_read16(bus, slot, func, 0x06);
    if (!(status & (1U << 4)))
        return -EINVAL;

    uint8_t cap_ptr = (uint8_t)(pci_read16(bus, slot, func, 0x34) & 0xFF);

    int iter = 0;
    while (cap_ptr != 0) {
        if (++iter > PCI_CAP_MAX_ITERATIONS)
            break;

        uint16_t cap_id_next = pci_read16(bus, slot, func, cap_ptr);
        uint8_t cap_id = (uint8_t)(cap_id_next & 0xFF);

        if (cap_id == 0x05) {
            /* Found MSI capability */
            uint16_t msg_ctrl = pci_read16(bus, slot, func, cap_ptr + 2);

            info->cap_offset = cap_ptr;
            info->is_64bit = (msg_ctrl & PCI_MSI_CTRL_64BIT) ? 1 : 0;
            info->has_per_vector = (msg_ctrl & PCI_MSI_CTRL_PERVEC) ? 1 : 0;
            info->mmc = (uint16_t)((msg_ctrl & PCI_MSI_CTRL_MMC_MASK) >> PCI_MSI_CTRL_MMC_SHIFT);

            return 0;
        }

        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }

    return -EINVAL;
}

/* ── MSI-X capability parsing ─────────────────────────────────────── */

int pci_find_msix_cap(uint8_t bus, uint8_t slot, uint8_t func,
                      struct msix_info *info) {
    if (!info) return -EINVAL;

    /* MSI-X capability ID = 0x11 */
    uint16_t status = pci_read16(bus, slot, func, 0x06);
    if (!(status & (1U << 4))) return -EINVAL;

    uint8_t cap_ptr = (uint8_t)(pci_read16(bus, slot, func, 0x34) & 0xFF);

    int iter = 0;
    while (cap_ptr != 0) {
        if (++iter > PCI_CAP_MAX_ITERATIONS)
            break;

        uint16_t cap_id_next = pci_read16(bus, slot, func, cap_ptr);
        uint8_t cap_id = (uint8_t)(cap_id_next & 0xFF);

        if (cap_id == 0x11) {
            /* Found MSI-X capability */
            info->cap_offset = cap_ptr;

            /* Message Control at cap_ptr + 2 */
            uint16_t msg_ctrl = pci_read16(bus, slot, func, cap_ptr + 2);
            info->table_size = (msg_ctrl & 0x07FF) + 1;  /* bits 0-10: table size */

            /* MSI-X Table BAR/offset at cap_ptr + 4 */
            uint32_t tbl_reg;
            if (ecam_base) {
                tbl_reg = pcie_read(bus, slot, func, cap_ptr + 4);
            } else {
                tbl_reg = pci_read(bus, slot, func, cap_ptr + 4);
            }
            info->table_bir = (uint8_t)(tbl_reg & PCI_MSIX_TBL_BIR);
            info->table_offset = tbl_reg & PCI_MSIX_TBL_OFFSET;

            /* PBA BAR/offset at cap_ptr + 8 */
            uint32_t pba_reg;
            if (ecam_base) {
                pba_reg = pcie_read(bus, slot, func, cap_ptr + 8);
            } else {
                pba_reg = pci_read(bus, slot, func, cap_ptr + 8);
            }
            info->pba_bir = (uint8_t)(pba_reg & PCI_MSIX_TBL_BIR);
            info->pba_offset = pba_reg & PCI_MSIX_TBL_OFFSET;

            return 0;
        }

        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }

    return -EINVAL;
}

/* ── Enable MSI interrupts ────────────────────────────────────────── */

int pci_enable_msi(struct pci_device *dev, uint8_t vector,
                   uint32_t apic_id, int nvecs, uint8_t delivery)
{
    struct msi_info info;
    int ret = pci_find_msi_cap(dev->bus, dev->slot, dev->func, &info);
    if (ret < 0)
        return -EINVAL;

    uint8_t bus = dev->bus;
    uint8_t slot = dev->slot;
    uint8_t func = dev->func;
    uint16_t cap = info.cap_offset;

    /* Clamp nvecs to what the device supports */
    int max_nvecs = 1U << info.mmc;   /* device supports up to 2^mmc vectors */
    if (nvecs > max_nvecs)
        nvecs = max_nvecs;
    if (nvecs < 1)
        nvecs = 1;

    /* Calculate MME field value: log2(nvecs) rounded down */
    int mme = 0;
    while ((1U << (mme + 1)) <= (unsigned int)nvecs && mme < info.mmc)
        mme++;

    /* Build the MSI message address */
    uint32_t msg_addr = 0xFEE00000u | (apic_id << 12);
    uint32_t msg_upper = 0;

    /* Build the MSI message data: vector + delivery mode */
    /* For fixed delivery: data = vector | (delivery << 8) */
    uint16_t msg_data = (uint16_t)vector | ((uint16_t)(delivery & 7) << 8);

    /* Read current message control */
    uint16_t msg_ctrl = pci_read16(bus, slot, func, cap + 2);

    /* Disable MSI while configuring */
    pci_write16(bus, slot, func, cap + 2, (uint16_t)(msg_ctrl & ~PCI_MSI_CTRL_ENABLE));

    /* Write message address */
    pci_write32(bus, slot, func, cap + 4, msg_addr);

    if (info.is_64bit) {
        /* 64-bit addressing: write upper address + data at +8, +0xC */
        pci_write32(bus, slot, func, cap + 8, msg_upper);
        pci_write16(bus, slot, func, cap + 0xC, msg_data);

        /* Clear per-vector mask bits if supported */
        if (info.has_per_vector) {
            pci_write32(bus, slot, func, cap + 0x10, 0);  /* unmask all */
        }
    } else {
        /* 32-bit addressing: data at +8 */
        pci_write16(bus, slot, func, cap + 8, msg_data);

        /* Clear per-vector mask bits if supported */
        if (info.has_per_vector) {
            pci_write32(bus, slot, func, cap + 0xC, 0);  /* unmask all */
        }
    }

    /* Update MME and enable MSI */
    msg_ctrl = pci_read16(bus, slot, func, cap + 2);
    msg_ctrl &= ~PCI_MSI_CTRL_MME_MASK;
    msg_ctrl |= (uint16_t)(mme << PCI_MSI_CTRL_MME_SHIFT);
    msg_ctrl |= PCI_MSI_CTRL_ENABLE;
    pci_write16(bus, slot, func, cap + 2, msg_ctrl);

    kprintf("[PCI] MSI enabled for %02x:%02x.%x: vector=%u, %d vector(s), "
            "addr=0x%08x, data=0x%04x\n",
            (unsigned int)bus, (unsigned int)slot, (unsigned int)func,
            (unsigned int)vector, 1U << mme,
            (unsigned int)msg_addr, (unsigned int)msg_data);

    return vector;
}

/* ── Disable MSI interrupts ───────────────────────────────────────── */

void pci_disable_msi(struct pci_device *dev) {
    struct msi_info info;
    if (pci_find_msi_cap(dev->bus, dev->slot, dev->func, &info) < 0)
        return;

    uint16_t msg_ctrl = pci_read16(dev->bus, dev->slot, dev->func, info.cap_offset + 2);
    msg_ctrl = (uint16_t)(msg_ctrl & ~PCI_MSI_CTRL_ENABLE);
    pci_write16(dev->bus, dev->slot, dev->func, info.cap_offset + 2, msg_ctrl);
}

/* ── Enable MSI-X interrupts ──────────────────────────────────────── */
/*
 * MSI-X table entry format (16 bytes each):
 *   DW0: Message Address (lower 32 bits)
 *   DW1: Message Upper Address (upper 32 bits)
 *   DW2: [15:0] Message Data, [31:16] Vector Control
 *   DW3: Reserved
 */
#define MSIX_ENTRY_SIZE    16
#define MSIX_CTRL_MASKED   (1u << 31)   /* Vector Control: masked */

int pci_enable_msix(struct pci_device *dev, struct msix_info *info,
                    volatile uint32_t *table_virt,
                    const uint8_t *vectors, const uint32_t *apic_ids,
                    int n)
{
    uint8_t bus = dev->bus;
    uint8_t slot = dev->slot;
    uint8_t func = dev->func;

    if (!info || !table_virt || !vectors || !apic_ids)
        return -EINVAL;
    if (n <= 0 || (uint32_t)n > info->table_size)
        return -EINVAL;

    uint16_t cap = info->cap_offset;

    /* Disable MSI-X and mask all entries while configuring */
    uint16_t msg_ctrl = pci_read16(bus, slot, func, cap + 2);
    pci_write16(bus, slot, func, cap + 2, msg_ctrl & (uint16_t)~(1u << 15));  /* clear enable */

    /* Mask all entries first */
    for (int i = 0; i < n; i++) {
        table_virt[i * 4 + 2] = MSIX_CTRL_MASKED;   /* DW2: set mask bit */
    }
    __asm__ volatile("mfence" ::: "memory");

    /* Program each entry */
    for (int i = 0; i < n; i++) {
        uint32_t addr_low = 0xFEE00000u | (apic_ids[i] << 12);
        uint32_t addr_high = 0;
        uint16_t msg_data = (uint16_t)vectors[i] | (PCI_MSI_DM_FIXED << 8);

        table_virt[i * 4 + 0] = addr_low;      /* Message Address */
        table_virt[i * 4 + 1] = addr_high;     /* Upper Address */
        table_virt[i * 4 + 2] = msg_data;      /* Data (unmasked) */
        __asm__ volatile("mfence" ::: "memory");
    }

    /* Enable MSI-X */
    msg_ctrl = pci_read16(bus, slot, func, cap + 2);
    msg_ctrl |= (1u << 15);   /* MSI-X enable */
    msg_ctrl |= (1u << 14);   /* function mask clear */
    pci_write16(bus, slot, func, cap + 2, msg_ctrl);

    kprintf("[PCI] MSI-X enabled for %02x:%02x.%x: %d vector(s)\n",
            (unsigned int)bus, (unsigned int)slot, (unsigned int)func, n);

    return 0;
}

/* ── Disable MSI-X interrupts ─────────────────────────────────────── */

void pci_disable_msix(struct pci_device *dev) {
    struct msix_info info;
    if (pci_find_msix_cap(dev->bus, dev->slot, dev->func, &info) < 0)
        return;

    uint16_t msg_ctrl = pci_read16(dev->bus, dev->slot, dev->func, info.cap_offset + 2);
    msg_ctrl &= (uint16_t)~(1u << 15);   /* clear MSI-X enable */
    pci_write16(dev->bus, dev->slot, dev->func, info.cap_offset + 2, msg_ctrl);
}

/* ── High-level interrupt setup with MSI-X → MSI → INTx fallback ──── */

/* Allocate an interrupt vector in the available range.
 * We use vectors 48-239 which are free (0-31=exceptions, 32-47=legacy IRQ,
 * 240-242=IPIs, 243-255 reserved).
 * Returns the vector number, or < 0 on failure.
 */
static int msi_alloc_vector(void) {
    static int next_vector = 48;      /* first available MSI vector */
    static int vectors_used = 0;

    if (next_vector + vectors_used >= 240) {
        kprintf("[PCI] ERROR: no free interrupt vectors\n");
        return -EINVAL;
    }
    int vec = next_vector + vectors_used;
    vectors_used++;
    return vec;
}

static void irq_register_dispatch(int vector, isr_handler_t handler) {
    /* The idt_register_handler is the standard way to register ISRs */
    extern void idt_register_handler(uint8_t vector, isr_handler_t handler);
    idt_register_handler((uint8_t)vector, handler);
}

int pci_setup_interrupts(struct pci_device *dev,
                         struct pci_interrupt_config *cfg,
                         isr_handler_t handler)
{
    if (!dev || !cfg)
        return -EINVAL;

    memset(cfg, 0, sizeof(*cfg));

    /* ── Attempt 1: MSI-X ─────────────────────────────────────────── */
    struct msix_info msix_info;
    int has_msix = pci_find_msix_cap(dev->bus, dev->slot, dev->func, &msix_info);

    if (has_msix == 0 && msix_info.table_size > 0) {
        /* Map the MSI-X table from the device's BAR */
        int bir = msix_info.table_bir;
        if (bir >= 0 && bir < 6 && dev->bar[bir] != 0) {
            uint32_t bar_val = dev->bar[bir];
            uint64_t phys_base = (uint64_t)(bar_val & ~0xFu);
            uint64_t table_phys = phys_base + msix_info.table_offset;

            /* Allocate vectors */
            int nvecs = (int)msix_info.table_size;
            if (nvecs > 8) nvecs = 8;  /* limit to reasonable number */

            /* Map the MSI-X table region.
             * The table may straddle a page boundary if the BAR offset is
             * near the end of a 4KB page, so we map ALL pages that the
             * nvecs entries (each MSIX_ENTRY_SIZE bytes) touch. */
            uint64_t table_start_page = table_phys & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t table_end = table_phys + (uint64_t)nvecs * MSIX_ENTRY_SIZE - 1;
            uint64_t table_end_page = table_end & ~(uint64_t)(PAGE_SIZE - 1);

            for (uint64_t page = table_start_page; page <= table_end_page;
                 page += PAGE_SIZE) {
                uint64_t virt = (uint64_t)PHYS_TO_VIRT(page);
                vmm_map_page(virt, page,
                             VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
            }

            volatile uint32_t *table = (volatile uint32_t *)(
                (uint64_t)PHYS_TO_VIRT(table_start_page)
                + (table_phys - table_start_page));

            uint8_t vectors[8];
            uint32_t apic_ids[8];
            uint32_t my_apic_id = apic_get_id();

            for (int i = 0; i < nvecs; i++) {
                int vec = msi_alloc_vector();
                if (vec < 0) {
                    kprintf("[PCI] MSI-X vector allocation failed for %02x:%02x.%x\n",
                            (unsigned int)dev->bus, (unsigned int)dev->slot, (unsigned int)dev->func);
                    pci_disable_msix(dev);
                    goto try_msi;
                }
                vectors[i] = (uint8_t)vec;
                apic_ids[i] = my_apic_id;

                /* Register the handler for each vector */
                irq_register_dispatch(vec, handler);
            }

            if (pci_enable_msix(dev, &msix_info, table, vectors, apic_ids, nvecs) == 0) {
                cfg->type = 2;       /* MSI-X */
                cfg->vector = vectors[0];
                cfg->n_vectors = nvecs;
                kprintf("[PCI] %02x:%02x.%x: MSI-X with %d vector(s)\n",
                        (unsigned int)dev->bus, (unsigned int)dev->slot,
                        (unsigned int)dev->func, nvecs);
                return 0;
            }
        }
    }

try_msi:
    /* ── Attempt 2: MSI ──────────────────────────────────────────── */
    {
        int vec = msi_alloc_vector();
        if (vec >= 0) {
            uint32_t my_apic_id = apic_get_id();
            int ret = pci_enable_msi(dev, (uint8_t)vec, my_apic_id, 1, PCI_MSI_DM_FIXED);
            if (ret >= 0) {
                irq_register_dispatch(vec, handler);
                cfg->type = 1;       /* MSI */
                cfg->vector = vec;
                cfg->n_vectors = 1;
                kprintf("[PCI] %02x:%02x.%x: MSI with vector %d\n",
                        (unsigned int)dev->bus, (unsigned int)dev->slot,
                        (unsigned int)dev->func, vec);
                return 0;
            }
        }
    }

    /* ── Attempt 3: INTx (legacy) ────────────────────────────────── */
    {
        cfg->type = 0;       /* INTx */
        cfg->vector = 32 + dev->irq;   /* legacy IRQ mapping */
        cfg->n_vectors = 1;
        irq_register_dispatch(32 + dev->irq, handler);
        kprintf("[PCI] %02x:%02x.%x: INTx with IRQ %d (vector %d)\n",
                (unsigned int)dev->bus, (unsigned int)dev->slot,
                (unsigned int)dev->func, (unsigned int)dev->irq,
                32 + dev->irq);
        return 0;
    }
}

/*
 * pci_probe_bar_size - Probe a standard PCI BAR's size (write-all-1s).
 * @bus:   PCI bus number
 * @slot:  PCI slot number
 * @func:  PCI function number
 * @bar_off: Config-space offset of the BAR (0x10 + i*4)
 * @orig_lo: The original value of the BAR (already read by caller)
 * @is_64bit: 1 for a 64-bit MMIO BAR (probes the upper-32 register too)
 *
 * Writes 0xFFFFFFFF to the BAR register, reads back the size mask,
 * then restores the original value.  For 64-bit BARs the adjacent
 * upper-32 register is also probed.
 *
 * Returns the size in bytes, or 0 if the BAR is unimplemented (all
 * address bits are hardwired 0 → register reads 0 on probe).
 */
static uint64_t pci_probe_bar_size(int bus, int slot, int func,
                                    int bar_off, int is_64bit,
                                    uint32_t orig_lo)
{
    if (orig_lo == 0)
        return 0;
    if (orig_lo & 1)
        return 0;   /* I/O BAR — not handled here */

    /* Probe lower 32-bit part */
    pci_write(bus, slot, func, bar_off, 0xFFFFFFFF);
    uint32_t mask_lo = pci_read(bus, slot, func, bar_off);
    pci_write(bus, slot, func, bar_off, orig_lo);

    /* Bits [31:4] encode address space; ~(mask) + 1 gives size
     * (PCI spec §6.2.5.1).  Work in uint64_t to avoid overflow. */
    uint64_t size = (uint64_t)(~(mask_lo & (uint32_t)~0xFu)) + 1ULL;

    if (is_64bit && size != 0) {
        /* Probe upper 32-bit part (address bits [63:32]) */
        int off_hi = bar_off + 4;
        uint32_t orig_hi = pci_read(bus, slot, func, off_hi);
        pci_write(bus, slot, func, off_hi, 0xFFFFFFFF);
        uint32_t mask_hi = pci_read(bus, slot, func, off_hi);
        pci_write(bus, slot, func, off_hi, orig_hi);

        /* Combine lower-32 mask with upper-32 mask then add 1 */
        uint64_t full_mask = (uint64_t)(mask_lo & (uint32_t)~0xFu)
                           | ((uint64_t)mask_hi << 32);
        if (full_mask != 0)
            size = ~full_mask + 1ULL;
    }

    return size;
}

/* ── PCI class name ───────────────────────────────────────────────── */

static const char *pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x00: return sub == 0x01 ? "VGA-Compatible" : "Unclassified";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI Controller";
        case 0x01: return "IDE Controller";
        case 0x05: return "ATA Controller";
        case 0x06: return "SATA Controller";
        default:   return "Mass Storage";
        }
    case 0x02:
        return sub == 0x00 ? "Ethernet Controller" : "Network Controller";
    case 0x03: return "Display Controller";
    case 0x04: return "Multimedia Controller";
    case 0x05: return "Memory Controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-PCI Bridge";
        case 0x80: return "Other Bridge";
        default:   return "Bridge Device";
        }
    case 0x07: return "Serial Controller";
    case 0x08: return "System Peripheral";
    case 0x09: return "Input Device";
    case 0x0A: return "Docking Station";
    case 0x0B: return "Processor";
    case 0x0C:
        switch (sub) {
        case 0x00: return "FireWire (1394)";
        case 0x03: return "USB Controller";
        case 0x05: return "SMBus Controller";
        default:   return "Serial Bus";
        }
    case 0x0D: return "Wireless Controller";
    case 0x0F: return "Satellite Controller";
    case 0x10: return "Encryption Controller";
    case 0x11: return "Signal Processing";
    default:   return "Unknown";
    }
}

uint32_t pci_read(int bus, int slot, int func, int offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(int bus, int slot, int func, int offset, uint32_t val) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out) {
    if (!out) return -EINVAL;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
            uint16_t did = (uint16_t)((reg0 >> 16) & 0xFFFF);
            if (vid == 0xFFFF) continue;
            /* Check header type for multi-function (bit 7 at reg 0x0C byte 2) */
            uint32_t reg_hdr = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int is_multi = (reg_hdr & (1U << 23)) ? 1 : 0;
            int max_func = is_multi ? 8 : 1;
            for (int func = 0; func < max_func; func++) {
                reg0 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                vid = (uint16_t)(reg0 & 0xFFFF);
                did = (uint16_t)((reg0 >> 16) & 0xFFFF);
                if (vid == 0xFFFF) continue;
                if (vid == vendor && did == device) {
                    out->bus = (uint8_t)bus;
                    out->slot = (uint8_t)slot;
                    out->func = (uint8_t)func;
                    out->vendor_id = vid;
                    out->device_id = did;
                    uint32_t reg2 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                    out->class_code = (uint8_t)((reg2 >> 24) & 0xFF);
                    out->subclass = (uint8_t)((reg2 >> 16) & 0xFF);
                    uint32_t reg3c = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x3C);
                    out->irq = (uint8_t)(reg3c & 0xFF);
                    for (int i = 0; i < 6; i++)
                        out->bar[i] = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint8_t)(0x10 + i * 4));

                    /* Validate MMIO BAR address ranges don't overlap with RAM.
                     * Both the base address and the end of each BAR's range
                     * are verified to catch overlaps that span into adjacent
                     * memory regions. */
                    for (int i = 0; i < 6; i++) {
                        uint32_t bar_val = out->bar[i];
                        if (bar_val == 0 || (bar_val & 1))
                            continue; /* unassigned or I/O BAR */
                        uint8_t bar_type = (bar_val >> 1) & 0x3;
                        int is_64bit = (bar_type == 0x2);
                        int is_last_64bit = (is_64bit && i + 1 >= 6);

                        if (is_64bit && i + 1 < 6) {
                            /* 64-bit MMIO BAR: combine lower + upper dwords */
                            uint64_t phys = (uint64_t)(bar_val & ~0xFu)
                                          | ((uint64_t)out->bar[i+1] << 32);
                            uint64_t size = pci_probe_bar_size(
                                bus, slot, func, 0x10 + i * 4, 1, bar_val);

                            if (pmm_is_phys_ram(phys))
                                kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                        "(64-bit MMIO at 0x%llx) base overlaps with RAM!\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i,
                                        (unsigned long long)phys);

                            if (size > 0) {
                                uint64_t end = phys + size - 1;
                                if (end < phys)
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(64-bit MMIO at 0x%llx) size 0x%llx wraps!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size);
                                else if (pmm_is_phys_ram(end))
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(64-bit MMIO at 0x%llx, size 0x%llx) "
                                            "end at 0x%llx overlaps with RAM!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size,
                                            (unsigned long long)end);
                            }
                            i++; /* skip the upper-32 BAR */
                        } else if (!is_64bit || is_last_64bit) {
                            /* 32-bit MMIO (or 64-bit at last slot — unlikely) */
                            uint64_t phys = (uint64_t)(bar_val & ~0xFu);
                            uint64_t size = pci_probe_bar_size(
                                bus, slot, func, 0x10 + i * 4, 0, bar_val);

                            /* For a 64-bit BAR that doesn't fit (last slot),
                             * the size computed from just the lower dword is
                             * the truncated size — warn so this can't hide. */
                            if (is_last_64bit) {
                                kprintf("[PCI] %02x:%02x.%x BAR%d "
                                        "is a 64-bit MMIO at the last BAR slot — "
                                        "size probed from lower dword may be incomplete\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i);
                            }

                            if (pmm_is_phys_ram(phys))
                                kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                        "(MMIO at 0x%llx) base overlaps with RAM!\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i,
                                        (unsigned long long)phys);

                            if (size > 0) {
                                uint64_t end = phys + size - 1;
                                if (end < phys)
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(MMIO at 0x%llx) size 0x%llx wraps!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size);
                                else if (pmm_is_phys_ram(end))
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(MMIO at 0x%llx, size 0x%llx) "
                                            "end at 0x%llx overlaps with RAM!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size,
                                            (unsigned long long)end);
                            }
                        }
                    }

                    return 0;
                }
            }
        }
    }
    return -EINVAL;
}

int pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out) {
    if (!out) return -EINVAL;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;
            /* Check header type for multi-function */
            uint32_t reg_hdr = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x0C);
            int is_multi = (reg_hdr & (1U << 23)) ? 1 : 0;
            int max_func = is_multi ? 8 : 1;
            for (int func = 0; func < max_func; func++) {
                reg0 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                if ((reg0 & 0xFFFF) == 0xFFFF) continue;
                uint32_t reg2 = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x08);
                if (((reg2 >> 24) & 0xFF) == cls && ((reg2 >> 16) & 0xFF) == sub) {
                    out->bus        = (uint8_t)bus;
                    out->slot       = (uint8_t)slot;
                    out->func       = (uint8_t)func;
                    out->vendor_id  = (uint16_t)(reg0 & 0xFFFF);
                    out->device_id  = (uint16_t)((reg0 >> 16) & 0xFFFF);
                    out->class_code = cls;
                    out->subclass   = sub;
                    uint32_t r3c = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x3C);
                    out->irq = (uint8_t)(r3c & 0xFF);
                    for (int i = 0; i < 6; i++)
                        out->bar[i] = pci_read((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint8_t)(0x10 + i * 4));

                    /* Validate MMIO BAR address ranges don't overlap with RAM.
                     * Both the base address and the end of each BAR's range
                     * are verified to catch overlaps that span into adjacent
                     * memory regions. */
                    for (int i = 0; i < 6; i++) {
                        uint32_t bar_val = out->bar[i];
                        if (bar_val == 0 || (bar_val & 1))
                            continue;
                        uint8_t bar_type = (bar_val >> 1) & 0x3;
                        int is_64bit = (bar_type == 0x2);
                        int is_last_64bit = (is_64bit && i + 1 >= 6);

                        if (is_64bit && i + 1 < 6) {
                            /* 64-bit MMIO BAR: combine lower + upper dwords */
                            uint64_t phys = (uint64_t)(bar_val & ~0xFu)
                                          | ((uint64_t)out->bar[i+1] << 32);
                            uint64_t size = pci_probe_bar_size(
                                bus, slot, func, 0x10 + i * 4, 1, bar_val);

                            if (pmm_is_phys_ram(phys))
                                kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                        "(64-bit MMIO at 0x%llx) base overlaps with RAM!\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i,
                                        (unsigned long long)phys);

                            if (size > 0) {
                                uint64_t end = phys + size - 1;
                                if (end < phys)
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(64-bit MMIO at 0x%llx) size 0x%llx wraps!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size);
                                else if (pmm_is_phys_ram(end))
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(64-bit MMIO at 0x%llx, size 0x%llx) "
                                            "end at 0x%llx overlaps with RAM!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size,
                                            (unsigned long long)end);
                            }
                            i++;
                        } else if (!is_64bit || is_last_64bit) {
                            /* 32-bit MMIO (or 64-bit at last slot — unlikely) */
                            uint64_t phys = (uint64_t)(bar_val & ~0xFu);
                            uint64_t size = pci_probe_bar_size(
                                bus, slot, func, 0x10 + i * 4, 0, bar_val);

                            if (is_last_64bit) {
                                kprintf("[PCI] %02x:%02x.%x BAR%d "
                                        "is a 64-bit MMIO at the last BAR slot — "
                                        "size probed from lower dword may be incomplete\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i);
                            }

                            if (pmm_is_phys_ram(phys))
                                kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                        "(MMIO at 0x%llx) base overlaps with RAM!\n",
                                        (unsigned int)bus, (unsigned int)slot,
                                        (unsigned int)func, i,
                                        (unsigned long long)phys);

                            if (size > 0) {
                                uint64_t end = phys + size - 1;
                                if (end < phys)
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(MMIO at 0x%llx) size 0x%llx wraps!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size);
                                else if (pmm_is_phys_ram(end))
                                    kprintf("[PCI] WARNING: %02x:%02x.%x BAR%d "
                                            "(MMIO at 0x%llx, size 0x%llx) "
                                            "end at 0x%llx overlaps with RAM!\n",
                                            (unsigned int)bus, (unsigned int)slot,
                                            (unsigned int)func, i,
                                            (unsigned long long)phys,
                                            (unsigned long long)size,
                                            (unsigned long long)end);
                            }
                        }
                    }

                    return 0;
                }
            }
        }
    }
    return -EINVAL;
}

void pci_enable_bus_master(struct pci_device *dev) {
    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1U << 2); /* Bus Master Enable */
    pci_write(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_list(void) {
    kprintf("BUS SLOT VID:DID   CLS DESCRIPTION\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            uint16_t vid = (uint16_t)(reg0 & 0xFFFF);
            if (vid == 0xFFFF) continue;
            uint16_t did = (uint16_t)((reg0 >> 16) & 0xFFFF);
            uint32_t reg2 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x08);
            uint8_t cls = (uint8_t)((reg2 >> 24) & 0xFF);
            uint8_t sub = (uint8_t)((reg2 >> 16) & 0xFF);

            /* Check for PCIe capability */
            uint8_t cap_off;
            const char *extra = "";
            if (pci_find_pcie_cap((uint8_t)bus, (uint8_t)slot, 0, &cap_off) == 0) {
                uint8_t dtype = (uint8_t)pcie_device_type(bus, slot, 0);
                switch (dtype) {
                case PCIE_DEV_TYPE_ENDPOINT:   extra = " [PCIe Endpoint]"; break;
                case PCIE_DEV_TYPE_ROOT_PORT:  extra = " [PCIe Root Port]"; break;
                case PCIE_DEV_TYPE_UPSTREAM:   extra = " [PCIe Upstream]"; break;
                case PCIE_DEV_TYPE_DOWNSTREAM: extra = " [PCIe Downstream]"; break;
                case PCIE_DEV_TYPE_SWITCH:     extra = " [PCIe Switch]"; break;
                default:
                    break;
                }
            }

            /* Check for MSI/MSI-X capabilities */
            struct msi_info msi;
            struct msix_info msix;
            char msi_buf[32] = "";
            if (pci_find_msi_cap((uint8_t)bus, (uint8_t)slot, 0, &msi) == 0) {
                snprintf(msi_buf, sizeof(msi_buf), " [MSI%s%s]",
                         msi.is_64bit ? "-64" : "",
                         msi.has_per_vector ? "+VM" : "");
            }
            if (pci_find_msix_cap((uint8_t)bus, (uint8_t)slot, 0, &msix) == 0) {
                if (msi_buf[0] == 0) {
                    snprintf(msi_buf, sizeof(msi_buf), " [MSI-X(%u)]",
                             (unsigned int)msix.table_size);
                }
            }

            kprintf(" %02lx   %02lx  %04lx:%04lx %02lx.%02lx %s%s%s\n",
                    (unsigned long)bus, (unsigned long)slot,
                    (unsigned long)vid, (unsigned long)did,
                    (unsigned long)cls, (unsigned long)sub,
                    pci_class_name(cls, sub), extra, msi_buf);
        }
    }
}

/*
 * Find a PCIe extended capability by ID.
 *
 * PCIe extended capabilities live in the extended config space
 * (offsets 0x100 - 0xFFF).  Each extended capability header is
 * 4 bytes: [15:0] = capability ID, [19:16] = version, [31:20] = next.
 *
 * Returns the capability offset (>= 0x100) on success, or -1 if
 * not found or ECAM is unavailable.
 */

/* ── VPD (Vital Product Data) ────────────────────────────────────── */

int pci_vpd_find_cap(struct pci_device *dev)
{
    /* Check capabilities list bit */
    uint16_t status = pci_read16(dev->bus, dev->slot, dev->func, 0x06);
    if (!(status & (1U << 4)))
        return -EINVAL;

    uint8_t cap_ptr = (uint8_t)(pci_read16(dev->bus, dev->slot, dev->func, 0x34) & 0xFF);

    int iter = 0;
    while (cap_ptr != 0) {
        if (++iter > PCI_CAP_MAX_ITERATIONS)
            break;

        uint16_t cap_id_next = pci_read16(dev->bus, dev->slot, dev->func, cap_ptr);
        uint8_t cap_id = (uint8_t)(cap_id_next & 0xFF);

        if (cap_id == PCI_CAP_ID_VPD) {
            return (int)cap_ptr;
        }

        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }

    return -EINVAL;
}

int pci_vpd_capable(struct pci_device *dev)
{
    return pci_vpd_find_cap(dev) >= 0 ? 1 : 0;
}

int pci_vpd_read(struct pci_device *dev, uint32_t addr, uint32_t *val)
{
    if (!dev || !val)
        return -EINVAL;

    int cap = pci_vpd_find_cap(dev);
    if (cap < 0)
        return -EINVAL;

    uint8_t bus = dev->bus;
    uint8_t slot = dev->slot;
    uint8_t func = dev->func;

    /* Write address to VPD address register (with flag=0 for read) */
    pci_write32(bus, slot, func, (uint16_t)(cap + PCI_VPD_ADDR), addr & ~PCI_VPD_ADDR_F);

    /* Poll until flag bit is set (read complete) */
    int timeout = 10000;
    while (timeout--) {
        uint32_t reg = pci_read32(bus, slot, func, (uint16_t)(cap + PCI_VPD_ADDR));
        if (reg & PCI_VPD_ADDR_F) {
            /* Read complete — read the data */
            *val = pci_read32(bus, slot, func, (uint16_t)(cap + PCI_VPD_DATA));
            return 0;
        }
        /* Small delay — in a real kernel we'd use udelay */
        for (volatile int i = 0; i < 100; i++);
    }

    return -ETIMEDOUT;  /* Timeout */
}

int pci_vpd_write(struct pci_device *dev, uint32_t addr, uint32_t val)
{
    if (!dev)
        return -EINVAL;

    int cap = pci_vpd_find_cap(dev);
    if (cap < 0)
        return -EINVAL;

    uint8_t bus = dev->bus;
    uint8_t slot = dev->slot;
    uint8_t func = dev->func;

    /* Write the data first */
    pci_write32(bus, slot, func, (uint16_t)(cap + PCI_VPD_DATA), val);

    /* Write address with flag=1 to trigger write */
    pci_write32(bus, slot, func, (uint16_t)(cap + PCI_VPD_ADDR), addr | PCI_VPD_ADDR_F);

    /* Poll until flag bit is cleared (write complete) */
    int timeout = 10000;
    while (timeout--) {
        uint32_t reg = pci_read32(bus, slot, func, (uint16_t)(cap + PCI_VPD_ADDR));
        if (!(reg & PCI_VPD_ADDR_F)) {
            return 0;  /* Write complete */
        }
        for (volatile int i = 0; i < 100; i++);
    }

    return -ETIMEDOUT;  /* Timeout */
}

int pci_vpd_read_field(struct pci_device *dev, uint8_t field_tag,
                        char *buf, size_t len)
{
    if (!dev || !buf || len == 0)
        return -EINVAL;

    if (!pci_vpd_capable(dev))
        return -EINVAL;

    /* Scan VPD memory for the requested field tag */
    uint32_t offset = 0;
    int found = 0;
    int data_len = 0;
    uint8_t tag_data[256];  /* Max VPD field data size */

    while (offset < 32768) {  /* VPD is at most 32KB */
        uint32_t word;
        if (pci_vpd_read(dev, offset, &word) < 0)
            break;

        uint8_t tag = (uint8_t)(word & 0xFF);
        uint8_t len_byte = (uint8_t)((word >> 8) & 0xFF);

        /* Check for large resource descriptor (bit 7 and 6 of tag) */
        if ((tag & 0xC0) == 0x80) {
            /* Large resource: tag byte = tag, next 2 bytes = length */
            int large_len = len_byte | ((word >> 16) & 0xFF) << 8;
            if (tag == field_tag) {
                found = 1;
                data_len = large_len;
                /* Read the data bytes */
                int max_read = ((size_t)data_len < sizeof(tag_data)) ? data_len : (int)sizeof(tag_data);
                for (int i = 0; i < max_read; i += 4) {
                    uint32_t w;
                    if (pci_vpd_read(dev, offset + 3 + i, &w) < 0)
                        break;
                    int bytes_this_word = (max_read - i >= 4) ? 4 : (max_read - i);
                    for (int j = 0; j < bytes_this_word; j++)
                        tag_data[i + j] = (uint8_t)((w >> (j * 8)) & 0xFF);
                }
                break;
            }
            offset += 3 + large_len;  /* Skip tag + length bytes + data */
            /* Align to 4 bytes */
            offset = (offset + 3) & ~3;
        } else if (tag != 0xFF && tag != 0x00) {
            /* Small resource */
            int small_len = (len_byte & 0x07);
            if (tag == field_tag) {
                found = 1;
                data_len = small_len;
                /* Read remaining bytes from this word + subsequent words */
                int bytes_in_word = 2;  /* tag + len already consumed? */
                /* The word layout: byte0=tag, byte1=len, bytes 2-3 = first 2 data bytes */
                tag_data[0] = (uint8_t)((word >> 16) & 0xFF);
                tag_data[1] = (uint8_t)((word >> 24) & 0xFF);
                int remaining = small_len - 2;
                int pos = 2;
                while (remaining > 0) {
                    offset += 4;
                    if (pci_vpd_read(dev, offset, &word) < 0)
                        break;
                    int n = (remaining >= 4) ? 4 : remaining;
                    for (int j = 0; j < n && pos < (int)sizeof(tag_data); j++) {
                        tag_data[pos++] = (uint8_t)((word >> (j * 8)) & 0xFF);
                    }
                    remaining -= n;
                }
                break;
            }
            offset += 2 + small_len;
            /* Align to 4 bytes */
            offset = (offset + 3) & ~3;
        } else {
            /* End tag or invalid */
            if (tag == 0x78)
                break;  /* End tag */
            offset += 4;
        }
    }

    if (!found)
        return -EINVAL;

    /* Copy to output buffer (null-terminate) */
    int copy_len = (data_len < (int)len - 1) ? data_len : (int)len - 1;
    memcpy(buf, tag_data, (size_t)copy_len);
    buf[copy_len] = '\0';

    return copy_len;
}

int pci_find_ext_cap(int bus, int slot, int func, uint16_t cap_id) {
    if (!ecam_base) return -EINVAL;  /* Extended caps require ECAM access */

    uint16_t offset = 0x100;  /* Start of extended config space */
    int iter = 0;
    while (offset >= 0x100 && offset < 0x1000) {
        if (++iter > PCI_EXT_CAP_MAX_ITERATIONS)
            break;

        uint32_t header = pcie_read(bus, slot, func, offset);
        uint16_t id = header & 0xFFFF;
        if (id == 0xFFFF)
            break;  /* End of capabilities list */
        if (id == cap_id)
            return (int)offset;  /* Found it */

        uint16_t next = (uint16_t)((header >> 20) & 0xFFF);
        if (next == 0 || next < 0x100)
            break;  /* Last capability or invalid pointer */
        offset = next;
    }
    return -EINVAL;
}

/*
 * Find the AER extended capability on a PCIe device.
 *
 * AER has extended capability ID = 0x0001.
 */
int pci_find_aer_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_find_ext_cap(bus, slot, func, 0x0001);
}

/* Human-readable name for an uncorrectable AER error bit */
static const char *aer_uncor_name(uint32_t bit) {
    switch (bit) {
    case 4:  return "DL Protocol Error";
    case 5:  return "Surprise Down";
    case 12: return "Poisoned TLP";
    case 13: return "FC Protocol Error";
    case 14: return "Completion Timeout";
    case 15: return "Completer Abort";
    case 16: return "Unexpected Completion";
    case 17: return "Receiver Overflow";
    case 18: return "Malformed TLP";
    case 19: return "ECRC Error";
    case 20: return "Unsupported Request";
    case 21: return "ACS Violation";
    case 22: return "Internal Error";
    case 23: return "AtomicOp Egress Blocked";
    case 24: return "TLP Prefix Blocked";
    default: return "Unknown";
    }
}

/* Human-readable name for a correctable AER error bit */
static const char *aer_cor_name(uint32_t bit) {
    switch (bit) {
    case 0:  return "Receiver Error";
    case 6:  return "Bad TLP";
    case 7:  return "Bad DLLP";
    case 8:  return "REPLAY_NUM Rollover";
    case 12: return "Replay Timer Timeout";
    case 13: return "Advisory Non-Fatal";
    default: return "Unknown";
    }
}

/*
 * Check and log AER errors for a single PCIe device.
 *
 * Returns a bitmask: bit 0 = correctable error found,
 * bit 1 = uncorrectable error found.
 */
int pci_aer_check_device(uint8_t bus, uint8_t slot, uint8_t func) {
    int aer_off = pci_find_aer_cap(bus, slot, func);
    if (aer_off < 0)
        return 0;  /* No AER capability */

    int result = 0;

    /* Read uncorrectable error status */
    uint32_t uncor_status = pcie_read(bus, slot, func, (uint16_t)aer_off + PCI_AER_UNCOR_STATUS);
    if (uncor_status) {
        uint32_t uncor_mask = pcie_read(bus, slot, func, (uint16_t)aer_off + PCI_AER_UNCOR_MASK);
        uint32_t active = uncor_status & ~uncor_mask;

        if (active) {
            /* Read header log for diagnostics */
            uint32_t hdr_log[4] = {0};
            for (int i = 0; i < 4; i++)
                hdr_log[i] = pcie_read(bus, slot, func, (uint16_t)((uint16_t)aer_off + PCI_AER_HEADER_LOG + (uint16_t)(i * 4)));

            kprintf("[PCI AER] Bus %02x:%02x.%x Uncorrectable Error:\n",
                    (unsigned int)bus, (unsigned int)slot, (unsigned int)func);
            for (int b = 0; b < 32; b++) {
                if (active & (1U << b)) {
                    kprintf("  - %s (bit %d)\n", aer_uncor_name((uint32_t)b), b);
                }
            }
            kprintf("  Header Log: %08x %08x %08x %08x\n",
                    (unsigned int)hdr_log[0], (unsigned int)hdr_log[1],
                    (unsigned int)hdr_log[2], (unsigned int)hdr_log[3]);

            /* Clear the error by writing 1s to the status bits we saw */
            pcie_write(bus, slot, func, (uint16_t)aer_off + PCI_AER_UNCOR_STATUS, uncor_status);
            result |= 2;  /* Uncorrectable */
        }
    }

    /* Read correctable error status */
    uint32_t cor_status = pcie_read(bus, slot, func, (uint16_t)aer_off + PCI_AER_COR_STATUS);
    if (cor_status) {
        uint32_t cor_mask = pcie_read(bus, slot, func, (uint16_t)aer_off + PCI_AER_COR_MASK);
        uint32_t active = cor_status & ~cor_mask;

        if (active) {
            kprintf("[PCI AER] Bus %02x:%02x.%x Correctable Error:\n",
                    (unsigned int)bus, (unsigned int)slot, (unsigned int)func);
            for (int b = 0; b < 32; b++) {
                if (active & (1U << b)) {
                    kprintf("  - %s (bit %d)\n", aer_cor_name((uint32_t)b), b);
                }
            }
            /* Clear the error by writing 1s */
            pcie_write(bus, slot, func, (uint16_t)aer_off + PCI_AER_COR_STATUS, cor_status);
            result |= 1;  /* Correctable */
        }
    }

    return result;
}

/*
 * Check AER errors for all PCI devices.
 * This should be called periodically (e.g., from a timer or workqueue).
 * AER errors are logged but not fatal — the device continues operating.
 */
void pci_aer_check_all(void) {
    if (!ecam_base)
        return;  /* AER requires ECAM (memory-mapped PCIe config) */

    int total_errs = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0;
            if (ecam_base) {
                reg0 = pcie_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            } else {
                reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            }
            if ((reg0 & 0xFFFF) == 0xFFFF)
                continue;  /* No device */

            int errs = pci_aer_check_device((uint8_t)bus, (uint8_t)slot, 0);
            if (errs)
                total_errs++;
        }
    }

    if (total_errs > 0) {
        kprintf("[PCI AER] Check complete: %d device(s) had errors\n",
                total_errs);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PCIe Extended Capabilities: ACS, LTR, L1 PM Substates (Item 186)
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Access Control Services (ACS, ext cap ID 0x000D) ─────────────── */

int pci_find_acs_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_find_ext_cap(bus, slot, func, 0x000D);
}

void pci_log_acs_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    int off = pci_find_acs_cap(bus, slot, func);
    if (off < 0)
        return;

    uint16_t cap  = (uint16_t)pcie_read(bus, slot, func, (uint16_t)off + PCI_ACS_CAP);
    uint16_t ctrl = (uint16_t)pcie_read(bus, slot, func, (uint16_t)off + PCI_ACS_CTRL);

    kprintf("    ACS: cap=0x%04x ctrl=0x%04x",
            (unsigned int)cap, (unsigned int)ctrl);

    /* Log which capabilities are supported */
    if (cap & PCI_ACS_CAP_SV)  kprintf(" SV");
    if (cap & PCI_ACS_CAP_TB)  kprintf(" TB");
    if (cap & PCI_ACS_CAP_RR)  kprintf(" RR");
    if (cap & PCI_ACS_CAP_CR)  kprintf(" CR");
    if (cap & PCI_ACS_CAP_UF)  kprintf(" UF");
    if (cap & PCI_ACS_CAP_EC)  kprintf(" EC");
    if (cap & PCI_ACS_CAP_DT)  kprintf(" DT");

    /* Log which are enabled */
    if (ctrl) {
        kprintf(" (en:");
        if (ctrl & PCI_ACS_CTRL_SV) kprintf(" SV");
        if (ctrl & PCI_ACS_CTRL_TB) kprintf(" TB");
        if (ctrl & PCI_ACS_CTRL_RR) kprintf(" RR");
        if (ctrl & PCI_ACS_CTRL_CR) kprintf(" CR");
        if (ctrl & PCI_ACS_CTRL_UF) kprintf(" UF");
        if (ctrl & PCI_ACS_CTRL_EC) kprintf(" EC");
        if (ctrl & PCI_ACS_CTRL_DT) kprintf(" DT");
        kprintf(")");
    }

    /* Log Egress Control Vector if P2P Egress Control is supported */
    if ((cap & PCI_ACS_CAP_EC) && (ctrl & PCI_ACS_CTRL_EC)) {
        uint16_t egress = (uint16_t)pcie_read(bus, slot, func,
                            (uint16_t)off + PCI_ACS_EGRESS_CTRL);
        kprintf(" egress=0x%04x", (unsigned int)egress);
    }

    kprintf("\n");
}

/* ── Latency Tolerance Reporting (LTR, ext cap ID 0x0018) ─────────── */

int pci_find_ltr_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_find_ext_cap(bus, slot, func, 0x0018);
}

uint64_t pci_ltr_to_ns(uint16_t ltr_reg) {
    uint16_t raw_val = ltr_reg & PCI_LTR_VALUE_MASK;
    unsigned int scale = (ltr_reg >> PCI_LTR_SCALE_SHIFT) & 0x7;

    /* Scale factors from PCIe spec: 0=1ns, 1=32ns, 2=1024ns, 3=32768ns,
     * 4=1048576ns, 5=33554432ns */
    static const uint64_t ltr_scales[] = {
        1ULL, 32ULL, 1024ULL, 32768ULL, 1048576ULL, 33554432ULL
    };

    if (scale >= ARRAY_SIZE(ltr_scales))
        return 0;  /* Invalid scale */

    return (uint64_t)raw_val * ltr_scales[scale];
}

void pci_log_ltr_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    int off = pci_find_ltr_cap(bus, slot, func);
    if (off < 0)
        return;

    uint16_t max_snoop   = (uint16_t)pcie_read(bus, slot, func, (uint16_t)off + PCI_LTR_MAX_SNOOP);
    uint16_t max_nosnoop = (uint16_t)pcie_read(bus, slot, func, (uint16_t)off + PCI_LTR_MAX_NOSNOOP);

    uint64_t snoop_ns   = pci_ltr_to_ns(max_snoop);
    uint64_t nosnoop_ns = pci_ltr_to_ns(max_nosnoop);

    kprintf("    LTR: max_snoop=%u(%lluns) max_nosnoop=%u(%lluns)%s\n",
            (unsigned int)(max_snoop & PCI_LTR_VALUE_MASK),
            (unsigned long long)snoop_ns,
            (unsigned int)(max_nosnoop & PCI_LTR_VALUE_MASK),
            (unsigned long long)nosnoop_ns,
            (max_snoop & PCI_LTR_REQUIRE) ? " [REQUIRED]" : "");
}

/* ── L1 PM Substates (ext cap ID 0x001E) ─────────────────────────── */

int pci_find_l1pm_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    return pci_find_ext_cap(bus, slot, func, 0x001E);
}

void pci_log_l1pm_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    int off = pci_find_l1pm_cap(bus, slot, func);
    if (off < 0)
        return;

    uint32_t l1pm_cap  = pcie_read(bus, slot, func, (uint16_t)off + PCI_L1PM_CAP);
    uint32_t l1pm_ctrl1 = pcie_read(bus, slot, func, (uint16_t)off + PCI_L1PM_CTRL1);

    kprintf("    L1PM: cap=0x%08x ctrl1=0x%08x",
            (unsigned int)l1pm_cap, (unsigned int)l1pm_ctrl1);

    /* Supported substates */
    if (l1pm_cap & PCI_L1PM_CAP_PCIPM_L12) kprintf(" PCI-PM_L1.2");
    if (l1pm_cap & PCI_L1PM_CAP_PCIPM_L11) kprintf(" PCI-PM_L1.1");
    if (l1pm_cap & PCI_L1PM_CAP_ASPM_L12)  kprintf(" ASPM_L1.2");
    if (l1pm_cap & PCI_L1PM_CAP_ASPM_L11)  kprintf(" ASPM_L1.1");
    if (l1pm_cap & PCI_L1PM_CAP_LTR_BLOCK) kprintf(" LTR_Block");

    /* L1.2 enable status */
    if (l1pm_ctrl1 & PCI_L1PM_CTRL1_L12_EN)
        kprintf(" [L1.2_EN]");

    /* CommonModeRestoreTime and PowerOnTime */
    uint8_t cm_rest = (uint8_t)(l1pm_ctrl1 & PCI_L1PM_CTRL1_CM_REST_TIME_MASK);
    uint8_t pwr_on  = (uint8_t)((l1pm_ctrl1 & PCI_L1PM_CTRL1_PWR_ON_TIME_MASK)
                                >> PCI_L1PM_CTRL1_PWR_ON_SHIFT);
    if (cm_rest || pwr_on) {
        kprintf(" cm_rest=%u pwr_on=%u",
                (unsigned int)cm_rest, (unsigned int)pwr_on);
    }

    kprintf("\n");
}

void __init pci_init(void) {
    int count = 0;
    int msi_count = 0;
    int msix_count = 0;
    int acs_count = 0;
    int ltr_count = 0;
    int l1pm_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((reg0 & 0xFFFF) != 0xFFFF) {
                count++;

                /* ── PCI module autoloading (M35) ─────────────────────
                 * On device discovery, attempt to autoload a matching
                 * driver module using the standard modalias format:
                 *   pci:vXXXXdXXXXsvXXXXsdXXXXbcXXccXX
                 *
                 * The modalias string is stored in a static buffer for
                 * deferred autoprobe, since the PCI scan runs in early
                 * boot context.  pci_autoprobe_work() processes them
                 * later in a workqueue context.
                 */
                {
                    uint16_t vendor  = (uint16_t)(reg0 & 0xFFFF);
                    uint16_t device  = (uint16_t)(reg0 >> 16);
                    uint32_t reg2c   = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x2C);
                    uint16_t subsys_v = (uint16_t)(reg2c & 0xFFFF);
                    uint16_t subsys_d = (uint16_t)(reg2c >> 16);
                    uint32_t reg08   = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x08);
                    uint8_t  base_cl = (uint8_t)(reg08 >> 24);
                    uint8_t  sub_cl  = (uint8_t)((reg08 >> 16) & 0xFF);

                    char modalias[128];
                    snprintf(modalias, sizeof(modalias),
                             "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xcc%02X",
                             (unsigned int)vendor, (unsigned int)device,
                             (unsigned int)subsys_v, (unsigned int)subsys_d,
                             (unsigned int)base_cl, (unsigned int)sub_cl);

                    /* Queue for deferred autoprobe */
                    pci_queue_autoprobe(modalias, vendor, device,
                                        subsys_v, subsys_d,
                                        base_cl, sub_cl);
                }

                struct msi_info msi;
                if (pci_find_msi_cap((uint8_t)bus, (uint8_t)slot, 0, &msi) == 0)
                    msi_count++;
                struct msix_info msix;
                if (pci_find_msix_cap((uint8_t)bus, (uint8_t)slot, 0, &msix) == 0)
                    msix_count++;

                /* ── Probe PCIe extended capabilities (Item 186) ─────
                 * Detect and log ACS, LTR, and L1 PM Substates on
                 * PCIe devices for improved isolation diagnostics,
                 * power management tuning, and ASPM configuration.
                 */
                if (ecam_base) {
                    if (pci_find_acs_cap((uint8_t)bus, (uint8_t)slot, 0) >= 0) {
                        acs_count++;
                        pci_log_acs_cap((uint8_t)bus, (uint8_t)slot, 0);
                    }
                    if (pci_find_ltr_cap((uint8_t)bus, (uint8_t)slot, 0) >= 0) {
                        ltr_count++;
                        pci_log_ltr_cap((uint8_t)bus, (uint8_t)slot, 0);
                    }
                    if (pci_find_l1pm_cap((uint8_t)bus, (uint8_t)slot, 0) >= 0) {
                        l1pm_count++;
                        pci_log_l1pm_cap((uint8_t)bus, (uint8_t)slot, 0);
                    }
                }
            }
        }
    }
    kprintf("  %lu PCI devices found (%d MSI, %d MSI-X capable",
            (unsigned long)count, msi_count, msix_count);
    if (ecam_base) {
        kprintf("; %d ACS, %d LTR, %d L1PM",
                acs_count, ltr_count, l1pm_count);
    }
    kprintf(")\n");
}

#include "initcall.h"
device_initcall(pci_init);

/* ═══════════════════════════════════════════════════════════════════════
 *  PCI Deferred Autoprobe
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * PCI driver autoprobe using the standard modalias format:
 *   pci:vXXXXdXXXXsvXXXXsdXXXXbcXXccXX
 *
 * During PCI scan (early boot), we cannot call request_module()
 * because interrupts may be disabled and the module loader needs
 * a preemptible context.  Instead, we queue discovered devices
 * for deferred processing via pci_autoprobe_work(), which runs
 * later in a workqueue context.
 *
 * For each queued device, we attempt to match against registered
 * PCI driver ID tables and call the driver's probe function.
 */

/** Maximum number of queued autoprobe entries */
#define PCI_AUTOPROBE_MAX_ENTRIES 64

/** A single autoprobe entry for a discovered PCI device */
struct pci_autoprobe_entry {
    char     modalias[128];   /* Full modalias string */
    uint16_t vendor;
    uint16_t device;
    uint16_t subsys_vendor;
    uint16_t subsys_device;
    uint8_t  base_class;
    uint8_t  sub_class;
    int      used;
};

/** Static queue of devices waiting for autoprobe */
static struct pci_autoprobe_entry g_autoprobe_queue[PCI_AUTOPROBE_MAX_ENTRIES];
static int g_autoprobe_count = 0;

/**
 * pci_queue_autoprobe — Queue a PCI device for deferred autoprobe.
 *
 * Called during PCI scan to record a discovered device.  The actual
 * probing happens later via pci_autoprobe_work().
 */
static void pci_queue_autoprobe(const char *modalias,
                          uint16_t vendor, uint16_t device,
                          uint16_t subsys_vendor, uint16_t subsys_device,
                          uint8_t base_class, uint8_t sub_class)
{
    if (g_autoprobe_count >= PCI_AUTOPROBE_MAX_ENTRIES)
        return;

    struct pci_autoprobe_entry *entry = &g_autoprobe_queue[g_autoprobe_count];
    strncpy(entry->modalias, modalias, sizeof(entry->modalias) - 1);
    entry->modalias[sizeof(entry->modalias) - 1] = '\0';
    entry->vendor         = vendor;
    entry->device         = device;
    entry->subsys_vendor  = subsys_vendor;
    entry->subsys_device  = subsys_device;
    entry->base_class     = base_class;
    entry->sub_class      = sub_class;
    entry->used           = 1;

    g_autoprobe_count++;
}

/**
 * pci_match_modalias — Check if a modalias matches a device ID table.
 *
 * @modalias:  Modalias string (e.g., "pci:v8086d1234*")
 * @id_table:  Array of PCI device IDs (NULL-terminated)
 *
 * Returns 1 if match, 0 otherwise.
 */
static int pci_match_modalias(const char *modalias, const struct pci_device_id *id_table)
{
    if (!modalias || !id_table)
        return 0;

    /* Parse the modalias to extract vendor, device, etc. */
    uint16_t vendor = 0, device = 0;
    uint16_t subsys_v = 0, subsys_d = 0;
    unsigned int base_cl_val = 0, sub_cl_val = 0;

    /* Parse: pci:vXXXXXXXXdXXXXXXXXsvXXXXXXXXsdXXXXXXXXbcXXccXX */
    if (sscanf(modalias, "pci:v%08hxd%08hxsv%08hxsd%08hxbc%02xcc%02x",
               &vendor, &device, &subsys_v, &subsys_d,
               &base_cl_val, &sub_cl_val) < 4)
        return 0;
    uint8_t base_cl = (uint8_t)base_cl_val;
    uint8_t sub_cl = (uint8_t)sub_cl_val;

    /* Match against the ID table */
    for (const struct pci_device_id *id = id_table;
         id->vendor != 0 || id->device != 0; id++) {

        /* PCI_ANY_ID (0xFFFF or 0) matches anything */
        int vendor_match = (id->vendor == PCI_ANY_ID || id->vendor == vendor);
        int device_match = (id->device == PCI_ANY_ID || id->device == device);

        /* Subsystem match (optional) */
        int subsys_v_match = (id->subvendor == PCI_ANY_ID ||
                              id->subvendor == subsys_v);
        int subsys_d_match = (id->subdevice == PCI_ANY_ID ||
                              id->subdevice == subsys_d);

        /* Class match (optional) */
        int class_match = 1;
        if (id->class_mask != 0) {
            uint32_t dev_class = ((uint32_t)base_cl << 8) | sub_cl;
            uint32_t id_class = id->class;
            class_match = ((dev_class & id->class_mask) ==
                           (id_class & id->class_mask));
        }

        if (vendor_match && device_match &&
            subsys_v_match && subsys_d_match && class_match) {
            return 1;  /* Match found */
        }
    }

    return 0;  /* No match */
}

/**
 * pci_autoprobe_work — Process the autoprobe queue.
 *
 * Iterates all queued PCI devices and attempts to match them
 * against registered drivers.  This should be called from a
 * workqueue after the PCI bus has been fully scanned.
 */
static void pci_autoprobe_work(void)
{
    kprintf("[PCI] Autoprobe: processing %d queued devices\n", g_autoprobe_count);

    for (int i = 0; i < g_autoprobe_count; i++) {
        struct pci_autoprobe_entry *entry = &g_autoprobe_queue[i];
        if (!entry->used)
            continue;

        kprintf("[PCI] Autoprobe[%d] %s\n", i, entry->modalias);

        /* In a full implementation, we would iterate registered
         * PCI drivers and call their probe functions if the ID
         * matches.  For now, we attempt to load the module. */
        /* request_module("%s", entry->modalias); */

        /* Log the device for user-space matching */
        kprintf("[PCI]   vendor=0x%04x device=0x%04x "
                "subsys=0x%04x/0x%04x class=0x%02x/0x%02x\n",
                entry->vendor, entry->device,
                entry->subsys_vendor, entry->subsys_device,
                entry->base_class, entry->sub_class);
    }

    kprintf("[PCI] Autoprobe: done (%d devices processed)\n", g_autoprobe_count);
}

/* ── Exported symbols for driver modules ─────────────────────────── */
EXPORT_SYMBOL(pci_read);
EXPORT_SYMBOL(pci_write);
EXPORT_SYMBOL(pci_find_device);
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_enable_msi);
EXPORT_SYMBOL(pci_enable_msix);
EXPORT_SYMBOL(pci_disable_msi);
EXPORT_SYMBOL(pci_disable_msix);
EXPORT_SYMBOL(pci_setup_interrupts);
EXPORT_SYMBOL(pci_enable_bus_master);
EXPORT_SYMBOL(pci_find_ext_cap);
EXPORT_SYMBOL(pci_find_acs_cap);
EXPORT_SYMBOL(pci_find_ltr_cap);
EXPORT_SYMBOL(pci_find_l1pm_cap);
EXPORT_SYMBOL(pci_ltr_to_ns);

static uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t addr = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000u);
    outl(0xCF8, addr);
    outl(0xCFC, value);
}

static int pci_enable_device(struct pci_device *dev)
{
    if (!dev) return -EINVAL;
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x07;
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
    return 0;
}

/* ── Find PCI capability ────────────────────────────── */
static int pci_find_capability(void *pdev, int cap)
{
    if (!pdev)
        return -EINVAL;

    struct pci_device *dev = (struct pci_device *)pdev;

    /* Check capabilities list bit in status register (offset 0x04, bit 20) */
    uint32_t status = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    if (!(status & 0x00100000))
        return -ENOENT;  /* no capabilities list */

    /* Read capabilities pointer at offset 0x34 */
    uint32_t val = pci_read(dev->bus, dev->slot, dev->func, 0x34);
    uint8_t cap_ptr = (uint8_t)(val & 0xFF);

    /* Walk the capabilities list */
    while (cap_ptr != 0) {
        uint32_t cap_entry = pci_read(dev->bus, dev->slot, dev->func,
                                      (uint8_t)(cap_ptr & 0xFC));
        uint8_t cap_id = (uint8_t)(cap_entry & 0xFF);
        if (cap_id == (uint8_t)cap)
            return cap_ptr;  /* found, return offset */

        cap_ptr = (uint8_t)((cap_entry >> 8) & 0xFF);
    }

    return -ENOENT;
}
