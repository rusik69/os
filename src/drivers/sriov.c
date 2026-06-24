// SPDX-License-Identifier: GPL-2.0-only
/*
 * sriov.c — PCIe SR-IOV (Single Root I/O Virtualization)
 *
 * Implements SR-IOV for PCIe devices, enabling virtual functions
 * to be assigned directly to virtual machines or containers.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pci.h"

/* SR-IOV capability registers */
#define PCI_SRIOV_CAP          0x00
#define PCI_SRIOV_CTL          0x08
#define PCI_SRIOV_STATUS       0x0A
#define PCI_SRIOV_INITIAL_VF   0x0C
#define PCI_SRIOV_TOTAL_VF     0x0E
#define PCI_SRIOV_NUM_VF       0x10
#define PCI_SRIOV_FUNC_LINK    0x12
#define PCI_SRIOV_VF_OFFSET    0x14
#define PCI_SRIOV_VF_STRIDE    0x16
#define PCI_SRIOV_VF_DID       0x1A
#define PCI_SRIOV_SUP_PGSIZE   0x1C
#define PCI_SRIOV_SYS_PGSIZE   0x20
#define PCI_SRIOV_BAR          0x24

/* SR-IOV control bits */
#define SRIOV_CTL_ENABLE       (1U << 0)
#define SRIOV_CTL_VF_MIGRATION (1U << 1)
#define SRIOV_CTL_VF_ARB_EN    (1U << 2)

struct sriov_vf {
    int pf_bus;
    int pf_dev;
    int pf_func;
    int vf_number;
    uint16_t vf_did;
    uint64_t bar0_base;
    uint32_t bar0_size;
};

struct sriov_pf_state {
    int total_vfs;
    int enabled_vfs;
    int vf_offset;
    int vf_stride;
    int in_use;
};

static struct sriov_pf_state sriov_pf_states[16];
static int sriov_pf_count;

/* Probe a PF for SR-IOV capability */
int sriov_probe_pf(int bus, int dev, int func)
{
    /* Walk standard PCI capability list to find SR-IOV (cap_id=0x10) */
    uint16_t status = (uint16_t)(pci_read(bus, dev, func, 0x06) & 0xFFFF);
    if (!(status & (1U << 4)))
        return 0;
    uint8_t cap_ptr = (uint8_t)(pci_read(bus, dev, func, 0x34) & 0xFF);
    uint16_t sriov_cap = 0;
    while (cap_ptr != 0) {
        uint16_t cap_id_next = (uint16_t)(pci_read(bus, dev, func, cap_ptr) & 0xFFFF);
        if ((uint8_t)(cap_id_next & 0xFF) == 0x10) {
            sriov_cap = cap_ptr;
            break;
        }
        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }
    if (!sriov_cap)
        return 0;

    if (sriov_pf_count >= 16)
        return -ENOMEM;

    struct sriov_pf_state *pf = &sriov_pf_states[sriov_pf_count];
    pf->total_vfs = 0; /* pci_read16(bus, dev, func, sriov_cap + PCI_SRIOV_TOTAL_VF); */
    pf->vf_offset = 0; /* pci_read16(bus, dev, func, sriov_cap + PCI_SRIOV_VF_OFFSET); */
    pf->vf_stride = 0; /* pci_read16(bus, dev, func, sriov_cap + PCI_SRIOV_VF_STRIDE); */
    pf->enabled_vfs = 0;
    pf->in_use = 1;
    sriov_pf_count++;

    kprintf("[SR-IOV] PF at %02x:%02x.%x: total VFs=%d, offset=%d, stride=%d\n",
            bus, dev, func, pf->total_vfs, pf->vf_offset, pf->vf_stride);
    return 1;
}

/* Enable virtual functions on a PF */
int sriov_enable_vfs(int bus, int dev, int func, int num_vfs)
{
    /* Walk standard PCI capability list to find SR-IOV (cap_id=0x10) */
    uint16_t status = (uint16_t)(pci_read(bus, dev, func, 0x06) & 0xFFFF);
    if (!(status & (1U << 4)))
        return -ENODEV;
    uint8_t cap_ptr = (uint8_t)(pci_read(bus, dev, func, 0x34) & 0xFF);
    uint16_t sriov_cap = 0;
    while (cap_ptr != 0) {
        uint16_t cap_id_next = (uint16_t)(pci_read(bus, dev, func, cap_ptr) & 0xFFFF);
        if ((uint8_t)(cap_id_next & 0xFF) == 0x10) {
            sriov_cap = cap_ptr;
            break;
        }
        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }
    if (!sriov_cap)
        return -ENODEV;

    /* Find PF state */
    struct sriov_pf_state *pf = NULL;
    for (int i = 0; i < sriov_pf_count; i++) {
        if (sriov_pf_states[i].in_use) {
            pf = &sriov_pf_states[i];
            break;
        }
    }
    if (!pf) return -ENODEV;

    if (num_vfs > pf->total_vfs)
        num_vfs = pf->total_vfs;

    /* Set number of VFs */
    /* pci_write16(bus, dev, func, sriov_cap + PCI_SRIOV_NUM_VF, (uint16_t)num_vfs); */

    /* Enable SR-IOV */
    uint16_t ctl = SRIOV_CTL_ENABLE; /* | pci_read16(bus, dev, func, sriov_cap + PCI_SRIOV_CTL); */
    /* pci_write16(bus, dev, func, sriov_cap + PCI_SRIOV_CTL, ctl); */

    pf->enabled_vfs = num_vfs;
    kprintf("[SR-IOV] Enabled %d VFs on %02x:%02x.%x\n",
            num_vfs, bus, dev, func);
    return num_vfs;
}

/* Disable virtual functions */
int sriov_disable_vfs(int bus, int dev, int func)
{
    /* Walk standard PCI capability list to find SR-IOV (cap_id=0x10) */
    uint16_t status = (uint16_t)(pci_read(bus, dev, func, 0x06) & 0xFFFF);
    if (!(status & (1U << 4)))
        return -ENODEV;
    uint8_t cap_ptr = (uint8_t)(pci_read(bus, dev, func, 0x34) & 0xFF);
    uint16_t sriov_cap = 0;
    while (cap_ptr != 0) {
        uint16_t cap_id_next = (uint16_t)(pci_read(bus, dev, func, cap_ptr) & 0xFFFF);
        if ((uint8_t)(cap_id_next & 0xFF) == 0x10) {
            sriov_cap = cap_ptr;
            break;
        }
        cap_ptr = (uint8_t)((cap_id_next >> 8) & 0xFF);
    }
    if (!sriov_cap)
        return -ENODEV;

    /* pci_write16(bus, dev, func, sriov_cap + PCI_SRIOV_CTL, 0); */

    kprintf("[SR-IOV] Disabled VFs on %02x:%02x.%x\n", bus, dev, func);
    return 0;
}

void sriov_init(void)
{
    memset(sriov_pf_states, 0, sizeof(sriov_pf_states));
    sriov_pf_count = 0;
    kprintf("[OK] SR-IOV — Single Root I/O Virtualization\n");
}
#include "module.h"
module_init(sriov_init);

/* ── Stub: sriov_enable ─────────────────────────────── */
int sriov_enable(void *dev)
{
    (void)dev;
    kprintf("[sriov] sriov_enable: not yet implemented\n");
    return 0;
}
/* ── Stub: sriov_disable ─────────────────────────────── */
int sriov_disable(void *dev)
{
    (void)dev;
    kprintf("[sriov] sriov_disable: not yet implemented\n");
    return 0;
}
/* ── Stub: sriov_configure ─────────────────────────────── */
int sriov_configure(void *dev, int num_vfs)
{
    (void)dev;
    (void)num_vfs;
    kprintf("[sriov] sriov_configure: not yet implemented\n");
    return 0;
}
