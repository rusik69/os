#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"

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

/* ── MSI-X capability parsing ─────────────────────────────────────── */

int pci_find_msix_cap(uint8_t bus, uint8_t slot, uint8_t func,
                      struct msix_info *info) {
    if (!info) return -1;

    /* MSI-X capability ID = 0x11 */
    /* Check capabilities list bit in status register */
    uint16_t status;
    if (ecam_base) {
        status = (uint16_t)pcie_read(bus, slot, func, 0x06);
    } else {
        status = (uint16_t)pci_read(bus, slot, func, 0x06);
    }

    if (!(status & (1 << 4))) return -1;

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

        if (cap_id == 0x11) {
            /* Found MSI-X capability */
            info->cap_offset = cap_ptr;

            /* Read Message Control at cap_ptr + 2 */
            uint16_t msg_ctrl;
            if (ecam_base) {
                msg_ctrl = (uint16_t)pcie_read(bus, slot, func, cap_ptr + 2);
            } else {
                msg_ctrl = (uint16_t)pci_read(bus, slot, func, cap_ptr + 2);
            }

            info->table_size = (msg_ctrl & 0x07FF) + 1;  /* bits 0-10: table size */

            /* Read MSI-X Table BAR/offset at cap_ptr + 4 */
            uint32_t tbl_reg;
            if (ecam_base) {
                tbl_reg = pcie_read(bus, slot, func, cap_ptr + 4);
            } else {
                tbl_reg = pci_read(bus, slot, func, cap_ptr + 4);
            }
            info->table_bir = tbl_reg & PCI_MSIX_TBL_BIR;
            info->table_offset = tbl_reg & PCI_MSIX_TBL_OFFSET;

            /* Read PBA BAR/offset at cap_ptr + 8 */
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

        cap_ptr = (cap_reg >> 8) & 0xFF;
    }

    return -1;
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

            kprintf(" %02x   %02x  %04x:%04x %02x.%02x %s%s\n",
                    (unsigned long)bus, (unsigned long)slot,
                    (unsigned long)vid, (unsigned long)did,
                    (unsigned long)cls, (unsigned long)sub,
                    pci_class_name(cls, sub), extra);
        }
    }
}

void pci_init(void) {
    int count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            if ((reg0 & 0xFFFF) != 0xFFFF) count++;
        }
    }
    kprintf("  %u PCI devices found\n", (unsigned long)count);
}
