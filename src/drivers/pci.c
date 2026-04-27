#include "pci.h"
#include "io.h"
#include "printf.h"

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

void pci_enable_bus_master(struct pci_device *dev) {
    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2); /* Bus Master Enable */
    pci_write(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_list(void) {
    kprintf("BUS SLOT VID:DID   CLASS\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t reg0 = pci_read(bus, slot, 0, 0);
            uint16_t vid = reg0 & 0xFFFF;
            if (vid == 0xFFFF) continue;
            uint16_t did = (reg0 >> 16) & 0xFFFF;
            uint32_t reg2 = pci_read(bus, slot, 0, 0x08);
            uint8_t cls = (reg2 >> 24) & 0xFF;
            uint8_t sub = (reg2 >> 16) & 0xFF;
            kprintf(" %02x   %02x  %04x:%04x %02x.%02x\n",
                    (uint64_t)bus, (uint64_t)slot,
                    (uint64_t)vid, (uint64_t)did,
                    (uint64_t)cls, (uint64_t)sub);
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
    kprintf("  %u PCI devices found\n", (uint64_t)count);
}
