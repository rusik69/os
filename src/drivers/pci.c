#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "apic.h"
#include "pmm.h"
#include "vmm.h"

/* PCIe ECAM (Memory-Mapped Configuration Space) */
static uint64_t ecam_base = 0;

void pcie_ecam_set_base(uint64_t base) {
    ecam_base = base;
}

int pcie_is_available(void) {
    return ecam_base != 0;
}

uint32_t pcie_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    if (!ecam_base) return pci_read(bus, slot, func, (uint8_t)(offset & 0xFF));
    uint64_t addr = ecam_base
                  | ((uint64_t)bus  << 20)
                  | ((uint64_t)slot << 15)
                  | ((uint64_t)func << 12)
                  | (offset & 0xFFC);
    return *(volatile uint32_t *)addr;
}

void pcie_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val) {
    if (!ecam_base) { pci_write(bus, slot, func, (uint8_t)(offset & 0xFF), val); return; }
    uint64_t addr = ecam_base
                  | ((uint64_t)bus  << 20)
                  | ((uint64_t)slot << 15)
                  | ((uint64_t)func << 12)
                  | (offset & 0xFFC);
    *(volatile uint32_t *)addr = val;
}

/* ── PCI Express capability detection ─────────────────────────────── */

int pci_find_pcie_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t *cap_offset) {
    /* PCI Express capability ID = 0x10 */
    /* Read status register at offset 0x06 to check Capabilities List bit */
    uint16_t status;
    if (ecam_base) {
        status = (uint16_t)pcie_read(bus, slot, func, 0x06);
    } else {
        status = (uint16_t)pci_read(bus, slot, func, 0x06);
    }

    if (!(status & (1 << 4))) {
        /* Capabilities list not present */
        return -1;
    }

    /* Read capabilities pointer at offset 0x34 */
    uint8_t cap_ptr;
    if (ecam_base) {
        cap_ptr = (uint8_t)(pcie_read(bus, slot, func, 0x34) & 0xFF);
    } else {
        cap_ptr = (uint8_t)(pci_read(bus, slot, func, 0x34) & 0xFF);
    }

    while (cap_ptr != 0) {
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

    return -1;
}

int pcie_is_present(void) {
    /* Scan for at least one PCIe device */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0;
            if (ecam_base) {
                reg0 = pcie_read(bus, slot, 0, 0);
            } else {
                reg0 = pci_read(bus, slot, 0, 0);
            }
            uint16_t vid = reg0 & 0xFFFF;
            if (vid == 0xFFFF) continue;

            uint8_t cap_off;
            if (pci_find_pcie_cap(bus, slot, 0, &cap_off) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

uint8_t pcie_device_type(uint8_t bus, uint8_t slot, uint8_t func) {
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
static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset) {
    if (ecam_base) {
        return (uint16_t)(pcie_read(bus, slot, func, offset) & 0xFFFF);
    }
    return (uint16_t)(pci_read(bus, slot, func, (uint8_t)(offset & 0xFF)) & 0xFFFF);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint16_t val) {
    uint32_t base;
    uint32_t mask = 0xFFFF0000u;
    if (ecam_base) {
        base = pcie_read(bus, slot, func, offset);
        pcie_write(bus, slot, func, offset, (base & mask) | val);
    } else {
        base = pci_read(bus, slot, func, (uint8_t)(offset & 0xFF));
        pci_write(bus, slot, func, (uint8_t)(offset & 0xFF), (base & mask) | val);
    }
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val) {
    if (ecam_base) {
        pcie_write(bus, slot, func, offset, val);
    } else {
        pci_write(bus, slot, func, (uint8_t)(offset & 0xFF), val);
    }
}

/* ── MSI capability parsing ───────────────────────────────────────── */

int pci_find_msi_cap(uint8_t bus, uint8_t slot, uint8_t func,
                     struct msi_info *info) {
    if (!info) return -1;

    /* Check capabilities list bit */
    uint16_t status = pci_read16(bus, slot, func, 0x06);
    if (!(status & (1 << 4)))
        return -1;

    uint8_t cap_ptr = (uint8_t)(pci_read16(bus, slot, func, 0x34) & 0xFF);

    while (cap_ptr != 0) {
        uint16_t cap_id_next = pci_read16(bus, slot, func, cap_ptr);
        uint8_t cap_id = cap_id_next & 0xFF;

        if (cap_id == 0x05) {
            /* Found MSI capability */
            uint16_t msg_ctrl = pci_read16(bus, slot, func, cap_ptr + 2);

            info->cap_offset = cap_ptr;
            info->is_64bit = (msg_ctrl & PCI_MSI_CTRL_64BIT) ? 1 : 0;
            info->has_per_vector = (msg_ctrl & PCI_MSI_CTRL_PERVEC) ? 1 : 0;
            info->mmc = (msg_ctrl & PCI_MSI_CTRL_MMC_MASK) >> PCI_MSI_CTRL_MMC_SHIFT;

            return 0;
        }

        cap_ptr = (cap_id_next >> 8) & 0xFF;
    }

    return -1;
}

/* ── MSI-X capability parsing ─────────────────────────────────────── */

int pci_find_msix_cap(uint8_t bus, uint8_t slot, uint8_t func,
                      struct msix_info *info) {
    if (!info) return -1;

    /* MSI-X capability ID = 0x11 */
    uint16_t status = pci_read16(bus, slot, func, 0x06);
    if (!(status & (1 << 4))) return -1;

    uint8_t cap_ptr = (uint8_t)(pci_read16(bus, slot, func, 0x34) & 0xFF);

    while (cap_ptr != 0) {
        uint16_t cap_id_next = pci_read16(bus, slot, func, cap_ptr);
        uint8_t cap_id = cap_id_next & 0xFF;

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
            info->table_bir = tbl_reg & PCI_MSIX_TBL_BIR;
            info->table_offset = tbl_reg & PCI_MSIX_TBL_OFFSET;

            /* PBA BAR/offset at cap_ptr + 8 */
            uint32_t pba_reg;
            if (ecam_base) {
                pba_reg = pcie_read(bus, slot, func, cap_ptr + 8);
            } else {
                pba_reg = pci_read(bus, slot, func, cap_ptr + 8);
            }
            info->pba_bir = pba_reg & PCI_MSIX_TBL_BIR;
            info->pba_offset = pba_reg & PCI_MSIX_TBL_OFFSET;

            return 0;
        }

        cap_ptr = (cap_id_next >> 8) & 0xFF;
    }

    return -1;
}

/* ── Enable MSI interrupts ────────────────────────────────────────── */

int pci_enable_msi(struct pci_device *dev, uint8_t vector,
                   uint32_t apic_id, int nvecs, uint8_t delivery)
{
    struct msi_info info;
    int ret = pci_find_msi_cap(dev->bus, dev->slot, dev->func, &info);
    if (ret < 0)
        return -1;

    uint8_t bus = dev->bus;
    uint8_t slot = dev->slot;
    uint8_t func = dev->func;
    uint16_t cap = info.cap_offset;

    /* Clamp nvecs to what the device supports */
    int max_nvecs = 1 << info.mmc;   /* device supports up to 2^mmc vectors */
    if (nvecs > max_nvecs)
        nvecs = max_nvecs;
    if (nvecs < 1)
        nvecs = 1;

    /* Calculate MME field value: log2(nvecs) rounded down */
    int mme = 0;
    while ((1 << (mme + 1)) <= nvecs && mme < info.mmc)
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
    pci_write16(bus, slot, func, cap + 2, msg_ctrl & ~PCI_MSI_CTRL_ENABLE);

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
            (unsigned int)vector, 1 << mme,
            (unsigned int)msg_addr, (unsigned int)msg_data);

    return vector;
}

/* ── Disable MSI interrupts ───────────────────────────────────────── */

void pci_disable_msi(struct pci_device *dev) {
    struct msi_info info;
    if (pci_find_msi_cap(dev->bus, dev->slot, dev->func, &info) < 0)
        return;

    uint16_t msg_ctrl = pci_read16(dev->bus, dev->slot, dev->func, info.cap_offset + 2);
    msg_ctrl &= ~PCI_MSI_CTRL_ENABLE;
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
        return -1;
    if (n <= 0 || (uint32_t)n > info->table_size)
        return -1;

    uint16_t cap = info->cap_offset;

    /* Disable MSI-X and mask all entries while configuring */
    uint16_t msg_ctrl = pci_read16(bus, slot, func, cap + 2);
    pci_write16(bus, slot, func, cap + 2, msg_ctrl & ~(1u << 15));  /* clear enable */

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
    msg_ctrl &= ~(1u << 15);   /* clear MSI-X enable */
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
        return -1;
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
        return -1;

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

            /* Map a page for the MSI-X table */
            uint64_t table_page = table_phys & ~(uint64_t)0xFFF;
            uint64_t table_virt = (uint64_t)PHYS_TO_VIRT(table_page);
            vmm_map_page(table_virt, table_page,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);

            volatile uint32_t *table = (volatile uint32_t *)(table_virt + (table_phys - table_page));

            /* Allocate vectors */
            int nvecs = (int)msix_info.table_size;
            if (nvecs > 8) nvecs = 8;  /* limit to reasonable number */

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

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            uint16_t vid = reg0 & 0xFFFF;
            uint16_t did = (reg0 >> 16) & 0xFFFF;
            if (vid == 0xFFFF) continue;
            if (vid == vendor && did == device) {
                out->bus = bus;
                out->slot = slot;
                out->func = 0;
                out->vendor_id = vid;
                out->device_id = did;
                uint32_t reg2 = pci_read(bus, slot, 0, 0x08);
                out->class_code = (reg2 >> 24) & 0xFF;
                out->subclass = (reg2 >> 16) & 0xFF;
                uint32_t reg3c = pci_read(bus, slot, 0, 0x3C);
                out->irq = reg3c & 0xFF;
                for (int i = 0; i < 6; i++)
                    out->bar[i] = pci_read(bus, slot, 0, 0x10 + i * 4);
                return 0;
            }
        }
    }
    return -1;
}

int pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t reg2 = pci_read(bus, slot, 0, 0x08);
            if (((reg2 >> 24) & 0xFF) == cls && ((reg2 >> 16) & 0xFF) == sub) {
                out->bus        = (uint8_t)bus;
                out->slot       = (uint8_t)slot;
                out->func       = 0;
                out->vendor_id  = reg0 & 0xFFFF;
                out->device_id  = (reg0 >> 16) & 0xFFFF;
                out->class_code = cls;
                out->subclass   = sub;
                uint32_t r3c = pci_read(bus, slot, 0, 0x3C);
                out->irq = r3c & 0xFF;
                for (int i = 0; i < 6; i++)
                    out->bar[i] = pci_read(bus, slot, 0, 0x10 + i * 4);
                return 0;
            }
        }
    }
    return -1;
}

void pci_enable_bus_master(struct pci_device *dev) {
    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2); /* Bus Master Enable */
    pci_write(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_list(void) {
    kprintf("BUS SLOT VID:DID   CLS DESCRIPTION\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            uint16_t vid = reg0 & 0xFFFF;
            if (vid == 0xFFFF) continue;
            uint16_t did = (reg0 >> 16) & 0xFFFF;
            uint32_t reg2 = pci_read(bus, slot, 0, 0x08);
            uint8_t cls = (reg2 >> 24) & 0xFF;
            uint8_t sub = (reg2 >> 16) & 0xFF;

            /* Check for PCIe capability */
            uint8_t cap_off;
            const char *extra = "";
            if (pci_find_pcie_cap(bus, slot, 0, &cap_off) == 0) {
                uint8_t dtype = pcie_device_type(bus, slot, 0);
                switch (dtype) {
                case PCIE_DEV_TYPE_ENDPOINT:   extra = " [PCIe Endpoint]"; break;
                case PCIE_DEV_TYPE_ROOT_PORT:  extra = " [PCIe Root Port]"; break;
                case PCIE_DEV_TYPE_UPSTREAM:   extra = " [PCIe Upstream]"; break;
                case PCIE_DEV_TYPE_DOWNSTREAM: extra = " [PCIe Downstream]"; break;
                case PCIE_DEV_TYPE_SWITCH:     extra = " [PCIe Switch]"; break;
                }
            }

            /* Check for MSI/MSI-X capabilities */
            struct msi_info msi;
            struct msix_info msix;
            char msi_buf[32] = "";
            if (pci_find_msi_cap(bus, slot, 0, &msi) == 0) {
                snprintf(msi_buf, sizeof(msi_buf), " [MSI%s%s]",
                         msi.is_64bit ? "-64" : "",
                         msi.has_per_vector ? "+VM" : "");
            }
            if (pci_find_msix_cap(bus, slot, 0, &msix) == 0) {
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

/* ── PCIe Advanced Error Reporting (AER) ────────────────────────── */

/*
 * Find the AER extended capability on a PCIe device.
 *
 * PCIe extended capabilities live in the extended config space
 * (offsets 0x100 - 0xFFF).  Each extended capability header is
 * 4 bytes: [15:0] = capability ID, [19:16] = version, [31:20] = next.
 *
 * AER has extended capability ID = 0x0001.
 */
int pci_find_aer_cap(uint8_t bus, uint8_t slot, uint8_t func) {
    if (!ecam_base) return -1;  /* AER requires ECAM access */

    uint16_t offset = 0x100;  /* Start of extended config space */
    while (offset < 0x1000) {
        uint32_t header = pcie_read(bus, slot, func, offset);
        uint16_t cap_id = header & 0xFFFF;
        if (cap_id == 0xFFFF)
            break;  /* No more capabilities */
        if (cap_id == 0x0001)
            return (int)offset;  /* Found AER capability */

        uint16_t next = (uint16_t)((header >> 20) & 0xFFF);
        if (next == 0)
            break;  /* Last capability */
        offset = next;
    }
    return -1;
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
            uint32_t hdr_log[4];
            for (int i = 0; i < 4; i++)
                hdr_log[i] = pcie_read(bus, slot, func, (uint16_t)aer_off + PCI_AER_HEADER_LOG + (uint16_t)(i * 4));

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

void pci_init(void) {
    int count = 0;
    int msi_count = 0;
    int msix_count = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            if ((reg0 & 0xFFFF) != 0xFFFF) {
                count++;
                struct msi_info msi;
                if (pci_find_msi_cap(bus, slot, 0, &msi) == 0)
                    msi_count++;
                struct msix_info msix;
                if (pci_find_msix_cap(bus, slot, 0, &msix) == 0)
                    msix_count++;
            }
        }
    }
    kprintf("  %lu PCI devices found (%d MSI, %d MSI-X capable)\n",
            (unsigned long)count, msi_count, msix_count);
}
