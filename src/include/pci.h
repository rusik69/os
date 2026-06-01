#ifndef PCI_H
#define PCI_H

#include "types.h"

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

/* MSI-X capability parsing */
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

#endif
