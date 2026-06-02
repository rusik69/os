#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "idt.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass;
    uint8_t  irq;
    uint32_t bar[6];
};

uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out);
int pci_find_class(uint8_t cls, uint8_t sub, struct pci_device *out);
void pci_enable_bus_master(struct pci_device *dev);
void pci_list(void);
void pci_init(void);

/* PCIe ECAM (Memory-mapped config space) */
void pcie_ecam_set_base(uint64_t base);   /* called from ACPI MCFG parser */
uint32_t pcie_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pcie_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val);
int pcie_is_available(void);

/* PCI Express capability detection */
int pci_find_pcie_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t *cap_offset);
int pcie_is_present(void);

/* PCI Express device type */
#define PCIE_DEV_TYPE_ENDPOINT    0
#define PCIE_DEV_TYPE_ROOT_PORT   1
#define PCIE_DEV_TYPE_UPSTREAM    2
#define PCIE_DEV_TYPE_DOWNSTREAM  3
#define PCIE_DEV_TYPE_SWITCH      4
#define PCIE_DEV_TYPE_UNKNOWN     0xFF

uint8_t pcie_device_type(uint8_t bus, uint8_t slot, uint8_t func);

/* ── MSI (Message Signalled Interrupts) ──────────────────────────── */

/* MSI capability ID = 0x05 */
/* Message Control register bits (at cap_offset + 2) */
#define PCI_MSI_CTRL_ENABLE    (1 << 0)
#define PCI_MSI_CTRL_MMC_SHIFT 1    /* Multiple Message Capable */
#define PCI_MSI_CTRL_MMC_MASK  (7 << 1)
#define PCI_MSI_CTRL_MME_SHIFT 4    /* Multiple Message Enable */
#define PCI_MSI_CTRL_MME_MASK  (7 << 4)
#define PCI_MSI_CTRL_64BIT     (1 << 7)  /* 64-bit addressing capable */
#define PCI_MSI_CTRL_PERVEC    (1 << 8)  /* Per-vector masking capable */

/* MSI delivery modes (used in data register) */
#define PCI_MSI_DM_FIXED       0x0
#define PCI_MSI_DM_LOWEST_PRI  0x1
#define PCI_MSI_DM_SMI         0x2
#define PCI_MSI_DM_NMI         0x4
#define PCI_MSI_DM_INIT        0x5
#define PCI_MSI_DM_EXTINT      0x7

/* MSI capability structure */
struct msi_info {
    uint16_t cap_offset;       /* MSI capability offset in config space */
    uint8_t  is_64bit;         /* 1 if 64-bit addressing supported */
    uint8_t  has_per_vector;   /* 1 if per-vector masking supported */
    uint16_t mmc;              /* Multiple Message Capable (log2 of count) */
};

/* Find and parse MSI capability. Returns 0 if found, -1 if not. */
int pci_find_msi_cap(uint8_t bus, uint8_t slot, uint8_t func,
                     struct msi_info *info);

/* Enable MSI interrupts for a device.
 * vector:      interrupt vector number
 * apic_id:     destination local APIC ID
 * nvecs:       number of vectors needed (rounded down to power of 2)
 * delivery:    delivery mode (PCI_MSI_DM_FIXED, etc.)
 * Returns the vector base on success, < 0 on error.
 */
int pci_enable_msi(struct pci_device *dev, uint8_t vector,
                   uint32_t apic_id, int nvecs, uint8_t delivery);

/* Disable MSI interrupts. */
void pci_disable_msi(struct pci_device *dev);

/* ── MSI-X (MSI with eXtended capabilities) ──────────────────────── */
#define PCI_MSIX_TBL_BIR       0x7   /* bits 0-2: BAR indicator */
#define PCI_MSIX_TBL_OFFSET    0xFFFFFFF8  /* bits 3-31: offset in BAR */

struct msix_info {
    uint16_t cap_offset;       /* MSI-X capability offset in config space */
    uint8_t  table_bir;        /* BAR index for MSI-X table */
    uint32_t table_offset;     /* offset within that BAR */
    uint32_t table_size;       /* number of MSI-X entries */
    uint8_t  pba_bir;          /* BAR index for PBA */
    uint32_t pba_offset;       /* offset within that BAR */
};

/* Find and parse MSI-X capability. Returns 0 if found, -1 if not. */
int pci_find_msix_cap(uint8_t bus, uint8_t slot, uint8_t func,
                      struct msix_info *info);

/* Enable MSI-X interrupts for a device.
 * info:        parsed MSI-X capability info
 * table_virt:  virtual address of the MSI-X table (must be mapped)
 * vectors:     array of vectors to program (one per table entry)
 * apic_ids:    array of destination APIC IDs (one per entry)
 * n:           number of entries to program (<= table_size)
 * Returns 0 on success, < 0 on error.
 */
int pci_enable_msix(struct pci_device *dev, struct msix_info *info,
                    volatile uint32_t *table_virt,
                    const uint8_t *vectors, const uint32_t *apic_ids,
                    int n);

/* Disable MSI-X interrupts. */
void pci_disable_msix(struct pci_device *dev);

/* ── High-level interrupt setup with fallback ────────────────────── */

/* Result of pci_setup_interrupts() */
struct pci_interrupt_config {
    int  type;                 /* 2 = MSI-X, 1 = MSI, 0 = INTx */
    int  vector;               /* base vector number */
    int  n_vectors;            /* number of vectors allocated */
};

/* Best-effort interrupt setup: tries MSI-X, then MSI, then INTx.
 * On success the handler at 'handler' will be registered for the vector(s).
 * Returns 0 on success, < 0 on total failure.
 */
int pci_setup_interrupts(struct pci_device *dev,
                         struct pci_interrupt_config *cfg,
                         isr_handler_t handler);

#endif
