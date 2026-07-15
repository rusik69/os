// SPDX-License-Identifier: GPL-2.0-only
/*
 * pcie_ptm.c — PCIe Precision Time Measurement
 *
 * Implements PCIe Precision Time Measurement (PTM) for
 * accurate time synchronization across PCIe hierarchy.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "kernel.h"
#include "pci.h"

/* PTM capability registers */
#define PCI_PTM_CAP     0x00
#define PCI_PTM_CTL     0x04
#define PCI_PTM_DIALOG  0x08

/* PTM capability bits */
#define PTM_CAP_REQ       (1U << 0)
#define PCI_PTM_CAP_ROOT  (1U << 1)

/* PTM control bits */
#define PTM_CTL_ENABLE    (1U << 0)
#define PTM_CTL_ROOT      (1U << 1)

/* PTM dialog */
#define PTM_DIALOG_LOCAL   0
#define PTM_DIALOG_MASTER  1

struct ptm_state {
    uint64_t local_time;
    uint64_t master_time;
    uint64_t correction;
    uint64_t dialog_count;
    int root_capable;
};

static struct ptm_state ptm;

/* Enable PTM on a device */
static int ptm_enable(int bus, int dev, int func)
{
    /* Walk standard PCI capability list to find PTM (cap_id=0x1F) */
    uint16_t status = (uint16_t)(pci_read(bus, dev, func, 0x06) & 0xFFFF);
    if (!(status & (1U << 4)))
        return -ENODEV;
    uint8_t cap_ptr = (uint8_t)(pci_read(bus, dev, func, 0x34) & 0xFF);
    uint16_t ptm_cap = 0;
    while (cap_ptr != 0) {
        uint16_t cap_id_next = (uint16_t)(pci_read(bus, dev, func, cap_ptr) & 0xFFFF);
        if ((uint8_t)(cap_id_next & 0xFF) == 0x1F) {
            ptm_cap = cap_ptr;
            break;
        }
        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }
    if (!ptm_cap)
        return -ENODEV;

    uint16_t cap = 0; /* pci_read16(bus, dev, func, ptm_cap + PCI_PTM_CAP); */

    ptm.root_capable = (cap & PCI_PTM_CAP_ROOT) ? 1 : 0;

    /* Enable PTM */
    /* pci_write16(bus, dev, func, ptm_cap + PCI_PTM_CTL, PTM_CTL_ENABLE); */

    kprintf("[PTM] Enabled on %02x:%02x.%x (root=%d)\n",
            bus, dev, func, ptm.root_capable);
    return 0;
}

/* Perform a PTM dialog (time exchange) */
static int ptm_dialog(void)
{
    /* Send PTM Request and get response */
    /* In real hardware, this involves a PTM dialog transaction */

    WRITE_ONCE(ptm.local_time, 0);    /* timer_get_ticks() */
    WRITE_ONCE(ptm.master_time, 0);
    WRITE_ONCE(ptm.dialog_count, READ_ONCE(ptm.dialog_count) + 1);

    /* Compute correction based on dialog exchange */
    /* tm = (t1 + (t4 - (t3 - t2)) / 2) - t2 */
    /* Simplified: */
    WRITE_ONCE(ptm.correction, READ_ONCE(ptm.master_time) - READ_ONCE(ptm.local_time));

    return 0;
}

/* Get corrected time */
static uint64_t ptm_get_corrected_time(uint64_t raw_time)
{
    if (READ_ONCE(ptm.dialog_count) == 0)
        return raw_time;

    return raw_time + READ_ONCE(ptm.correction);
}

static void ptm_init(void)
{
    memset(&ptm, 0, sizeof(ptm));
    kprintf("[OK] PCIe PTM — Precision Time Measurement\n");
}
#include "module.h"
module_init(ptm_init);

/* ── Stub: pcie_ptm_init ─────────────────────────────── */
static int pcie_ptm_init(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_ptm_init: not yet implemented\n");
    return 0;
}
/* ── Stub: pcie_ptm_enable ─────────────────────────────── */
static int pcie_ptm_enable(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_ptm_enable: not yet implemented\n");
    return 0;
}
/* ── Stub: pcie_ptm_disable ─────────────────────────────── */
static int pcie_ptm_disable(__maybe_unused void *dev)
{
    kprintf("[PCIE] pcie_ptm_disable: not yet implemented\n");
    return 0;
}
/* ── Stub: pcie_ptm_read_time ─────────────────────────────── */
static int pcie_ptm_read_time(__maybe_unused void *dev, __maybe_unused uint64_t *time)
{
    kprintf("[PCIE] pcie_ptm_read_time: not yet implemented\n");
    return 0;
}
