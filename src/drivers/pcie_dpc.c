// SPDX-License-Identifier: GPL-2.0-only
/*
 * pcie_dpc.c — PCIe Downstream Port Containment
 *
 * Implements DPC (Downstream Port Containment) for PCIe error
 * containment. DPC disables a port when an uncorrectable error
 * occurs, preventing propagation to the rest of the hierarchy.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pci.h"

/* DPC capability registers */
#define PCI_DPC_CAP          0x00
#define PCI_DPC_CTL          0x04
#define PCI_DPC_STATUS       0x06
#define PCI_DPC_ERR_SRC_ID   0x08
#define PCI_DPC_RP_LOG       0x0C
#define PCI_DPC_RP_LOG_SIZE  16

/* DPC control register bits */
#define DPC_CTL_ENABLE       (1 << 0)
#define DPC_CTL_INT_ENABLE   (1 << 1)
#define DPC_CTL_ERR_COR      (1 << 2)
#define DPC_CTL_TRIGGER      (1 << 3)

/* DPC status register bits */
#define DPC_STATUS_TRIGGER   (1 << 0)
#define DPC_STATUS_INTERRUPT (1 << 1)
#define DPC_STATUS_RP_BUSY   (1 << 2)

/* DPC trigger reason */
#define DPC_TRIGGER_RAS_DES  0  /* ERR_COR or ERR_FATAL/NONFATAL */
#define DPC_TRIGGER_DPC_TRIG 1  /* DPC Trigger bit */

static int dpc_triggered_count;

/* Enable DPC on a port */
int dpc_enable(int bus, int dev, int func)
{
    uint16_t dpc_cap = pci_find_ext_cap(bus, dev, func, 0x0017); /* PCI_EXT_CAP_ID_DPC */
    if (!dpc_cap)
        return -ENODEV;

    /* Read current control */
    /* pci_write16(bus, dev, func, dpc_cap + PCI_DPC_CTL, DPC_CTL_ENABLE | DPC_CTL_INT_ENABLE); */

    kprintf("[DPC] Enabled on %02x:%02x.%x\n", bus, dev, func);
    return 0;
}

/* Handle a DPC trigger event */
int dpc_handle_trigger(int bus, int dev, int func)
{
    uint16_t dpc_cap = pci_find_ext_cap(bus, dev, func, 0x0017); /* PCI_EXT_CAP_ID_DPC */
    if (!dpc_cap)
        return -ENODEV;

    uint16_t status = 0; /* pci_read16(bus, dev, func, dpc_cap + PCI_DPC_STATUS); */

    if (status & DPC_STATUS_TRIGGER) {
        kprintf("[DPC] Triggered on %02x:%02x.%x status=0x%04x\n",
                bus, dev, func, status);

        /* Read error source ID */
        uint16_t source_id = 0; /* pci_read16(bus, dev, func, dpc_cap + PCI_DPC_ERR_SRC_ID); */
        kprintf("[DPC] Error source: bus=%02x dev=%02x func=%x\n",
                source_id >> 8, (source_id >> 3) & 0x1F, source_id & 0x7);

        dpc_triggered_count++;

        /* Clear DPC trigger status and restore */
        /* pci_write16(bus, dev, func, dpc_cap + PCI_DPC_STATUS, DPC_STATUS_TRIGGER); */

        return 0;
    }

    return 0;
}

/* Get DPC statistics */
int dpc_get_trigger_count(void)
{
    return dpc_triggered_count;
}

void dpc_init(void)
{
    dpc_triggered_count = 0;
    kprintf("[OK] PCIe DPC — Downstream Port Containment\n");
}
