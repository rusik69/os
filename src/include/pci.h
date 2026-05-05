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

#endif
