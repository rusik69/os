// SPDX-License-Identifier: GPL-2.0-only
/*
 * pcie_aer.c — PCIe Advanced Error Reporting handler
 *
 * Handles PCIe Advanced Error Reporting (AER) events including
 * correctable and uncorrectable errors, error source identification,
 * and recovery actions.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pci.h"

/* AER capability registers */
#define PCI_ERR_CAP          0x00  /* AER Capability */
#define PCI_ERR_UNCOR_STATUS 0x04  /* Uncorrectable Error Status */
#define PCI_ERR_UNCOR_MASK   0x08  /* Uncorrectable Error Mask */
#define PCI_ERR_UNCOR_SEVER  0x0C  /* Uncorrectable Error Severity */
#define PCI_ERR_COR_STATUS   0x10  /* Correctable Error Status */
#define PCI_ERR_COR_MASK     0x14  /* Correctable Error Mask */
#define PCI_ERR_CAP_CTL      0x18  /* AER Capability & Control */
#define PCI_ERR_HEADER_LOG   0x1C  /* Header Log (4 DWORDs) */
#define PCI_ERR_ROOT_CMD     0x2C  /* Root Error Command */
#define PCI_ERR_ROOT_STATUS  0x30  /* Root Error Status */
#define PCI_ERR_COR_SRC      0x34  /* Error Source Identification (cor) */
#define PCI_ERR_UNCOR_SRC    0x36  /* Error Source Identification (uncor) */

/* Error severity */
#define AER_CORRECTABLE      0
#define AER_UNCORRECTABLE    1

struct aer_error {
    int bus;
    int dev;
    int func;
    uint32_t uncor_status;
    uint32_t cor_status;
};

static struct aer_error aer_errors[64];
static int aer_error_count;

/* Log an AER error */
void aer_log_error(int bus, int dev, int func,
                    uint32_t uncor_status, uint32_t cor_status)
{
    if (aer_error_count < 64) {
        aer_errors[aer_error_count].bus = bus;
        aer_errors[aer_error_count].dev = dev;
        aer_errors[aer_error_count].func = func;
        aer_errors[aer_error_count].uncor_status = uncor_status;
        aer_errors[aer_error_count].cor_status = cor_status;
        aer_error_count++;
    }

    kprintf("[AER] Error on %02x:%02x.%x uncor=0x%08x cor=0x%08x\n",
            bus, dev, func, uncor_status, cor_status);
}

/* Handle a correctable error */
int aer_handle_correctable(int bus, int dev, int func, uint32_t status)
{
    kprintf("[AER] Correctable: %02x:%02x.%x status=0x%08x\n",
            bus, dev, func, status);

    /* Clear status (in real hw, write 1 to clear) */
    uint16_t aer_cap = pci_find_ext_cap(bus, dev, func, 0x0001);
    if (aer_cap) {
        /* pci_write32(bus, dev, func, aer_cap + PCI_ERR_COR_STATUS, status); */
    }

    aer_log_error(bus, dev, func, 0, status);
    return 0; /* Correctable errors are recovered */
}

/* Handle an uncorrectable error */
int aer_handle_uncorrectable(int bus, int dev, int func, uint32_t status)
{
    kprintf("[AER] Uncorrectable: %02x:%02x.%x status=0x%08x\n",
            bus, dev, func, status);

    aer_log_error(bus, dev, func, status, 0);

    /* Check severity */
    /* Non-fatal may be recoverable, fatal requires reset */

    return -EIO;
}

/* Scan for AER capability on a device */
int aer_probe_device(int bus, int dev, int func)
{
    uint16_t aer_cap = pci_find_ext_cap(bus, dev, func, 0x0001); /* PCI_EXT_CAP_ID_AER */
    if (aer_cap) {
        kprintf("[AER] Found AER on %02x:%02x.%x (cap=0x%04x)\n",
                bus, dev, func, aer_cap);
        return 1;
    }
    return 0;
}

void aer_init(void)
{
    memset(aer_errors, 0, sizeof(aer_errors));
    aer_error_count = 0;
    kprintf("[OK] PCIe AER — Advanced Error Reporting handler\n");
}
#include "module.h"
module_init(aer_init);

/* ── Stub: pcie_aer_init ─────────────────────────────── */
int pcie_aer_init(void *dev)
{
    (void)dev;
    kprintf("[pcie] pcie_aer_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: pcie_aer_handler ─────────────────────────────── */
int pcie_aer_handler(void *dev, uint32_t status)
{
    (void)dev;
    (void)status;
    kprintf("[pcie] pcie_aer_handler: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: pcie_aer_clear ─────────────────────────────── */
int pcie_aer_clear(void *dev)
{
    (void)dev;
    kprintf("[pcie] pcie_aer_clear: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: pcie_aer_print ─────────────────────────────── */
int pcie_aer_print(void *dev)
{
    (void)dev;
    kprintf("[pcie] pcie_aer_print: not yet implemented\n");
    return -ENOSYS;
}
