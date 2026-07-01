/*
 * src/drivers/vmxnet3.c — VMware vmxnet3 Ethernet driver
 *
 * Implements the VMware vmxnet3 paravirtualized NIC.
 * Uses PCI vendor 0x15AD, device 0x07B0.
 * Provides MMIO-based register access and netdevice registration.
 *
 * Task 8: PCI device identification (this file skeleton)
 * Task 9: TX/RX descriptor rings
 */

#include "vmxnet3.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "netdevice.h"
#include "errno.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Static driver state ─────────────────────────────────────────── */
static struct vmxnet3_priv vmxnet3_state;

/* ── PCI probe ───────────────────────────────────────────────────── */

int vmxnet3_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(VMXNET3_VENDOR, VMXNET3_DEVICE, pci_dev);
    if (ret < 0)
        return -ENODEV;

    return 0;
}

int vmxnet3_probe(struct vmxnet3_priv *priv)
{
    struct pci_device pci_dev;
    uint64_t mmio_phys;
    uint32_t vr0, vr1;
    int ret;

    /* Find the vmxnet3 on the PCI bus */
    ret = vmxnet3_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  vmxnet3: device not found\n");
        return ret;
    }

    /* BAR0 contains the MMIO base address (the lower bits encode
     * the BAR type — mask them off to get the physical address). */
    mmio_phys = (uint64_t)(pci_dev.bar[0] & ~0xFULL);
    if (mmio_phys == 0) {
        kprintf("  vmxnet3: invalid MMIO base from BAR0\n");
        return -ENODEV;
    }

    priv->mmio_phys  = mmio_phys;
    priv->mmio_base  = (uintptr_t)PHYS_TO_VIRT((void *)(uintptr_t)mmio_phys);
    priv->irq_line   = pci_dev.irq;
    priv->present    = 0;
    priv->ifindex    = -1;

    /* Enable PCI bus mastering so the device can DMA to/from host memory */
    pci_enable_bus_master(&pci_dev);

    /* Read version registers to confirm the device is alive */
    vr0 = vmxnet3_readl(priv, VMXNET3_REG_VR0);
    vr1 = vmxnet3_readl(priv, VMXNET3_REG_VR1);

    kprintf("  vmxnet3: found at %02x:%02x.%d, "
            "MMIO phys 0x%llx (virt 0x%lx), IRQ %d, "
            "vr=0x%08x/0x%08x\n",
            pci_dev.bus, pci_dev.slot, pci_dev.func,
            (unsigned long long)priv->mmio_phys,
            (unsigned long)priv->mmio_base,
            priv->irq_line, vr0, vr1);

    /* Check magic version values */
    if (vr0 != VMXNET3_MAGIC_VR0) {
        kprintf("  vmxnet3: unexpected VR0 magic 0x%08x "
                "(expected 0x%08x)\n", vr0, VMXNET3_MAGIC_VR0);
        /* Continue anyway — older/future revisions may differ */
    }

    priv->present    = 1;
    return 0;
}

/* ── Module / device init entry point ────────────────────────────── */

int vmxnet3_init(void)
{
    int ret;

    memset(&vmxnet3_state, 0, sizeof(vmxnet3_state));
    vmxnet3_state.ifindex = -1;

    ret = vmxnet3_probe(&vmxnet3_state);
    if (ret < 0)
        return ret;

    kprintf("vmxnet3: driver loaded\n");
    return 0;
}

void vmxnet3_exit(void)
{
    if (vmxnet3_state.present) {
        vmxnet3_state.present = 0;
    }
    kprintf("vmxnet3: driver unloaded\n");
}

#ifdef MODULE
module_init(vmxnet3_init);
module_exit(vmxnet3_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("VMware vmxnet3 Ethernet driver (PCI device identification)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v000015ADd000007B0*");
#endif
