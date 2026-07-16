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
#define PCI_DPC_CTL          0x06
#define PCI_DPC_STATUS       0x08
#define PCI_DPC_ERR_SRC_ID   0x0A
#define PCI_DPC_RP_LOG       0x0C
#define PCI_DPC_RP_LOG_SIZE  16

/* DPC control register bits */
#define DPC_CTL_ENABLE       (1U << 0)
#define DPC_CTL_INT_ENABLE   (1U << 1)
#define DPC_CTL_ERR_COR      (1U << 2)
#define DPC_CTL_TRIGGER      (1U << 3)

/* DPC status register bits */
#define DPC_STATUS_TRIGGER   (1U << 0)
#define DPC_STATUS_INTERRUPT (1U << 1)
#define DPC_STATUS_RP_BUSY   (1U << 2)

/* DPC trigger reason */
#define DPC_TRIGGER_RAS_DES  0  /* ERR_COR or ERR_FATAL/NONFATAL */
#define DPC_TRIGGER_DPC_TRIG 1  /* DPC Trigger bit */

static int dpc_triggered_count = 0;

/* Enable DPC on a port */
static int dpc_enable(int bus, int dev, int func)
{
    uint16_t dpc_cap = (uint16_t)pci_find_ext_cap(bus, dev, func, 0x0017); /* PCI_EXT_CAP_ID_DPC */
    if (!dpc_cap)
        return -ENODEV;

    /* Enable DPC and DPC interrupt */
    pci_write16(bus, dev, func, dpc_cap + PCI_DPC_CTL, DPC_CTL_ENABLE | DPC_CTL_INT_ENABLE);

    kprintf("[DPC] Enabled on %02x:%02x.%x\n", bus, dev, func);
    return 0;
}

/* Handle a DPC trigger event */
static int dpc_handle_trigger(int bus, int dev, int func)
{
    uint16_t dpc_cap = (uint16_t)pci_find_ext_cap(bus, dev, func, 0x0017); /* PCI_EXT_CAP_ID_DPC */
    if (!dpc_cap)
        return -ENODEV;

    uint16_t status = pci_read16(bus, dev, func, dpc_cap + PCI_DPC_STATUS);

    if (status & DPC_STATUS_TRIGGER) {
        kprintf("[DPC] Triggered on %02x:%02x.%x status=0x%04x\n",
                bus, dev, func, status);

        /* Read error source ID */
        uint16_t source_id = pci_read16(bus, dev, func, dpc_cap + PCI_DPC_ERR_SRC_ID);
        kprintf("[DPC] Error source: bus=%02x dev=%02x func=%x\n",
                source_id >> 8, (source_id >> 3) & 0x1F, source_id & 0x7);

        dpc_triggered_count++;

        /* Clear DPC trigger status, interrupt status, and restore */
        pci_write16(bus, dev, func, dpc_cap + PCI_DPC_STATUS,
                    DPC_STATUS_TRIGGER | DPC_STATUS_INTERRUPT);

        return 0;
    }

    return 0;
}

/* Get DPC statistics */
static int dpc_get_trigger_count(void)
{
    return dpc_triggered_count;
}

static void dpc_init(void)
{
    dpc_triggered_count = 0;
    kprintf("[OK] PCIe DPC — Downstream Port Containment\n");
}
#include "module.h"
module_init(dpc_init);

/* ── Stub: pcie_dpc_init ─────────────────────────────── */
static int pcie_dpc_init(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_dpc_init: not yet implemented\n");
    return 0;
}
/* ── Stub: pcie_dpc_handler ─────────────────────────────── */
static int pcie_dpc_handler(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_dpc_handler: not yet implemented\n");
    return 0;
}
/* ── Stub: pcie_dpc_reset ─────────────────────────────── */
static int pcie_dpc_reset(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_dpc_reset: not yet implemented\n");
    return 0;
}
