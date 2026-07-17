// SPDX-License-Identifier: GPL-2.0-only
/*
 * sriov.c — PCIe SR-IOV (Single Root I/O Virtualization)
 *
 * Implements SR-IOV for PCIe devices, enabling virtual functions
 * to be assigned directly to virtual machines or containers.
 *
 * SR-IOV is a PCIe Extended Capability (cap_id = 0x0010) found
 * in the extended config space (offset >= 0x100), NOT in the
 * standard PCI capability list where cap_id 0x10 is PCI Express.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pci.h"

/* ── Forward declarations ─────────────────────────────────────────── */
int sriov_probe_pf(int bus, int dev, int func);
int sriov_enable_vfs(int bus, int dev, int func, int num_vfs);
int sriov_disable_vfs(int bus, int dev, int func);

/* SR-IOV extended capability ID */
#define PCI_EXT_CAP_ID_SRIOV    0x0010

/* SR-IOV capability register offsets
 * (relative to the SR-IOV extended capability base) */
#define PCI_SRIOV_CAP          0x00    /* Extended Capability Header */
#define PCI_SRIOV_CTL          0x08    /* SR-IOV Control (16-bit) */
#define PCI_SRIOV_STATUS       0x0A    /* SR-IOV Status (16-bit) */
#define PCI_SRIOV_INITIAL_VF   0x0C    /* Initial VFs (16-bit) */
#define PCI_SRIOV_TOTAL_VF     0x0E    /* Total VFs (16-bit) */
#define PCI_SRIOV_NUM_VF       0x10    /* Num VFs (16-bit) */
#define PCI_SRIOV_FUNC_LINK    0x12    /* Function Dependency Link (8-bit) */
#define PCI_SRIOV_VF_OFFSET    0x14    /* VF Offset (16-bit) */
#define PCI_SRIOV_VF_STRIDE    0x16    /* VF Stride (16-bit) */
#define PCI_SRIOV_VF_DID       0x1A    /* VF Device ID (16-bit) */
#define PCI_SRIOV_SUP_PGSIZE   0x1C    /* Supported Page Size (32-bit) */
#define PCI_SRIOV_SYS_PGSIZE   0x20    /* System Page Size (32-bit) */
#define PCI_SRIOV_BAR          0x24    /* VF BAR0 (32-bit, up to BAR5 at +0x38) */
#define PCI_SRIOV_VF_BAR_SIZE  4       /* Each VF BAR is 4 bytes */
#define PCI_SRIOV_MAX_VF_BARS  6       /* VF BAR0 through BAR5 */

/* SR-IOV control bits */
#define SRIOV_CTL_ENABLE       (1U << 0)
#define SRIOV_CTL_VF_MIGRATION (1U << 1)
#define SRIOV_CTL_VF_ARB_EN    (1U << 2)

/* VF BAR type bits */
#define PCI_BAR_MEM_SPACE      (0U << 0)
#define PCI_BAR_IO_SPACE       (1U << 0)
#define PCI_BAR_MEM_TYPE_MASK  (6U << 1)
#define PCI_BAR_MEM_TYPE_32    (0U << 1)
#define PCI_BAR_MEM_TYPE_64    (4U << 1)
#define PCI_BAR_PREFETCH       (1U << 3)
#define PCI_BAR_MEM_ATTR_MASK  0x0F
#define PCI_BAR_SIZE_MASK      0xFFFFFFF0  /* 32-bit BAR */

struct sriov_vf_bar {
    uint64_t base;
    uint64_t size;
    int      is_64bit;
    int      is_io;
};

struct sriov_vf {
    int pf_bus;
    int pf_dev;
    int pf_func;
    int vf_number;
    uint16_t vf_did;
    struct sriov_vf_bar bars[PCI_SRIOV_MAX_VF_BARS];
};

struct sriov_pf_state {
    int total_vfs;
    int enabled_vfs;
    int vf_offset;
    int vf_stride;
    int pf_bus;
    int pf_dev;
    int pf_func;
    int sriov_cap_offset;   /* Extended capability base offset */
    int in_use;
};

static struct sriov_pf_state sriov_pf_states[16];
static int sriov_pf_count = 0;

/*
 * Helper: find the SR-IOV extended capability on a device.
 * Returns the capability offset (>= 0x100) or < 0 if not found.
 */
static int sriov_find_cap(int bus, int dev, int func)
{
    return pci_find_ext_cap(bus, dev, func, PCI_EXT_CAP_ID_SRIOV);
}

/*
 * Size a single VF BAR register.
 * Writes 0xFFFFFFFF to determine size, then restores the original value.
 * Handles 64-bit BARs by sizing two adjacent slots.
 *
 * Returns the BAR size in bytes (power-of-2) or 0 if the BAR is unimplemented.
 */
static uint64_t sriov_size_vf_bar(int bus, int dev, int func,
                                   int cap_off, int bar_idx,
                                   int *is_64bit, int *is_io)
{
    int off = cap_off + PCI_SRIOV_BAR + bar_idx * PCI_SRIOV_VF_BAR_SIZE;

    /* Read original value */
    uint32_t orig_lo = pcie_read(bus, dev, func, off);

    if (orig_lo == 0) {
        *is_64bit = 0;
        *is_io = 0;
        return 0;  /* BAR not implemented */
    }

    *is_io = (orig_lo & PCI_BAR_IO_SPACE) ? 1 : 0;
    *is_64bit = 0;

    if (!*is_io) {
        uint8_t mem_type = (uint8_t)((orig_lo & PCI_BAR_MEM_TYPE_MASK) >> 1);
        if (mem_type == 2)  /* 0b10 = 64-bit */
            *is_64bit = 1;
    }

    uint64_t size;

    if (*is_64bit) {
        /*
         * 64-bit BAR sizing: write ALL-1s to BOTH halves, read back the
         * full 64-bit address mask, restore both originals, then compute
         * size via ~mask + 1 in a single step.
         *
         * Doing the low half first and the high half separately (the old
         * approach) is incorrect because ~sz_lo + 1 already converts the
         * low mask into a size — OR'ing in ~sz_hi << 32 and adding 1 again
         * produces a garbage result.  The combined ~full_mask + 1 must be
         * done exactly once.
         */
        int off_hi = off + PCI_SRIOV_VF_BAR_SIZE;
        uint32_t orig_hi = pcie_read(bus, dev, func, off_hi);

        pcie_write(bus, dev, func, off,   0xFFFFFFFF);
        pcie_write(bus, dev, func, off_hi, 0xFFFFFFFF);
        uint32_t sz_lo = pcie_read(bus, dev, func, off);
        uint32_t sz_hi = pcie_read(bus, dev, func, off_hi);

        pcie_write(bus, dev, func, off,   orig_lo);
        pcie_write(bus, dev, func, off_hi, orig_hi);

        /* Form the full 64-bit address mask then compute size exactly once */
        uint64_t full_mask = ((uint64_t)sz_hi << 32)
                           | (uint64_t)(sz_lo & PCI_BAR_SIZE_MASK);
        if (full_mask == 0 || full_mask == UINT64_MAX)
            return 0;               /* no address bits decoded (unimplemented) */
                                    /* or all bits decoded (degenerate)       */
        size = (~full_mask) + 1;
    } else {
        /* Standard 32-bit (or I/O) BAR sizing */
        pcie_write(bus, dev, func, off, 0xFFFFFFFF);
        uint32_t sz_lo = pcie_read(bus, dev, func, off);
        pcie_write(bus, dev, func, off, orig_lo);

        if (*is_io) {
            /* I/O BAR: bits 31:2 encode size */
            uint32_t io_mask = sz_lo & 0xFFFFFFFC;
            if (io_mask == 0)
                return 0;           /* I/O BAR not implemented */
            size = (uint64_t)(~io_mask) + 1;
        } else {
            /* Memory BAR: bits 31:4 encode size */
            uint32_t mem_mask = sz_lo & PCI_BAR_SIZE_MASK;
            if (mem_mask == 0)
                return 0;           /* memory BAR not implemented */
            size = (uint64_t)(~mem_mask) + 1;
        }
    }

    return size;
}

/*
 * Probe a PF for SR-IOV capability and read all capability registers,
 * including VF BAR sizing.
 *
 * Returns 1 on success, 0 if SR-IOV not supported, < 0 on error.
 */
int sriov_probe_pf(int bus, int dev, int func)
{
    /* Find SR-IOV extended capability (ID 0x0010) */
    int sriov_cap = sriov_find_cap(bus, dev, func);
    if (sriov_cap < 0)
        return 0;   /* No SR-IOV capability */

    if (sriov_pf_count >= 16)
        return -ENOMEM;

    /* Check capabilities list bit is set */
    uint16_t status = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x06);
    if (!(status & (1U << 4)))
        return 0;

    struct sriov_pf_state *pf = &sriov_pf_states[sriov_pf_count];
    pf->total_vfs    = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                  (uint16_t)(sriov_cap + PCI_SRIOV_TOTAL_VF));
    pf->vf_offset    = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                  (uint16_t)(sriov_cap + PCI_SRIOV_VF_OFFSET));
    pf->vf_stride    = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                  (uint16_t)(sriov_cap + PCI_SRIOV_VF_STRIDE));
    pf->pf_bus       = bus;
    pf->pf_dev       = dev;
    pf->pf_func      = func;
    pf->sriov_cap_offset = sriov_cap;
    pf->enabled_vfs  = 0;
    pf->in_use       = 1;
    sriov_pf_count++;

    /* Read supported page size and VF device ID */
    uint32_t sup_pgsize = pcie_read(bus, dev, func, sriov_cap + PCI_SRIOV_SUP_PGSIZE);
    uint16_t vf_did = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                                 (uint16_t)(sriov_cap + PCI_SRIOV_VF_DID));

    /* Size all VF BARs */
    kprintf("[SR-IOV] PF at %02x:%02x.%x: total VFs=%d, offset=%d, stride=%d\n",
            bus, dev, func, pf->total_vfs, pf->vf_offset, pf->vf_stride);

    uint64_t total_vf_mem = 0;
    for (int i = 0; i < PCI_SRIOV_MAX_VF_BARS; i++) {
        int is_64bit = 0, is_io = 0;
        uint64_t bar_sz = sriov_size_vf_bar(bus, dev, func,
                                            sriov_cap, i, &is_64bit, &is_io);
        if (bar_sz > 0) {
            kprintf("[SR-IOV]   VF BAR%d: %lld bytes %s%s\n",
                    i, (unsigned long long)bar_sz,
                    is_64bit ? "64-bit " : "32-bit ",
                    is_io ? "I/O" : "mem");
            total_vf_mem += bar_sz;
        }
        if (is_64bit) {
            /* 64-bit BAR consumes two slots; skip the next index */
            i++;
        }
    }

    kprintf("[SR-IOV]   Total memory required per VF: %lld bytes\n",
            (unsigned long long)total_vf_mem);
    kprintf("[SR-IOV]   VF Device ID: 0x%04x, Supported Page Size: 0x%08x\n",
            vf_did, sup_pgsize);

    return 1;
}

/* Enable virtual functions on a PF */
int sriov_enable_vfs(int bus, int dev, int func, int num_vfs)
{
    /* Find SR-IOV extended capability */
    int sriov_cap = sriov_find_cap(bus, dev, func);
    if (sriov_cap < 0)
        return -ENODEV;

    /* Find matching PF state by bus/dev/func */
    struct sriov_pf_state *pf = NULL;
    for (int i = 0; i < sriov_pf_count; i++) {
        if (sriov_pf_states[i].in_use &&
            sriov_pf_states[i].pf_bus  == bus &&
            sriov_pf_states[i].pf_dev  == dev &&
            sriov_pf_states[i].pf_func == func) {
            pf = &sriov_pf_states[i];
            break;
        }
    }
    if (!pf) return -ENODEV;

    if (num_vfs > pf->total_vfs)
        num_vfs = pf->total_vfs;

    /* Set number of VFs */
    pci_write16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                (uint16_t)(sriov_cap + PCI_SRIOV_NUM_VF), (uint16_t)num_vfs);

    /* Enable SR-IOV: read-modify-write the control register */
    uint16_t ctl = pci_read16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                              (uint16_t)(sriov_cap + PCI_SRIOV_CTL));
    ctl |= SRIOV_CTL_ENABLE;
    pci_write16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                (uint16_t)(sriov_cap + PCI_SRIOV_CTL), ctl);

    pf->enabled_vfs = num_vfs;
    kprintf("[SR-IOV] Enabled %d VFs on %02x:%02x.%x\n",
            num_vfs, bus, dev, func);
    return num_vfs;
}

/* Disable virtual functions */
int sriov_disable_vfs(int bus, int dev, int func)
{
    /* Find SR-IOV extended capability */
    int sriov_cap = sriov_find_cap(bus, dev, func);
    if (sriov_cap < 0)
        return -ENODEV;

    /* Disable SR-IOV by clearing control register */
    pci_write16((uint8_t)bus, (uint8_t)dev, (uint8_t)func,
                (uint16_t)(sriov_cap + PCI_SRIOV_CTL), 0);

    /* Clear the matching PF state */
    for (int i = 0; i < sriov_pf_count; i++) {
        if (sriov_pf_states[i].in_use &&
            sriov_pf_states[i].pf_bus  == bus &&
            sriov_pf_states[i].pf_dev  == dev &&
            sriov_pf_states[i].pf_func == func) {
            sriov_pf_states[i].enabled_vfs = 0;
            break;
        }
    }

    kprintf("[SR-IOV] Disabled VFs on %02x:%02x.%x\n", bus, dev, func);
    return 0;
}

static void sriov_init(void)
{
    memset(sriov_pf_states, 0, sizeof(sriov_pf_states));
    sriov_pf_count = 0;
    kprintf("[OK] SR-IOV — Single Root I/O Virtualization\n");
}
#include "module.h"
module_init(sriov_init);

/* ── Stub: sriov_enable ─────────────────────────────── */
static int sriov_enable(void *dev)
{
    (void)dev;
    kprintf("[SRIOV] sriov_enable: not yet implemented\n");
    return 0;
}
/* ── Stub: sriov_disable ─────────────────────────────── */
static int sriov_disable(void *dev)
{
    (void)dev;
    kprintf("[SRIOV] sriov_disable: not yet implemented\n");
    return 0;
}
/* ── Stub: sriov_configure ─────────────────────────────── */
static int sriov_configure(void *dev, int num_vfs)
{
    (void)dev;
    (void)num_vfs;
    kprintf("[SRIOV] sriov_configure: not yet implemented\n");
    return 0;
}
