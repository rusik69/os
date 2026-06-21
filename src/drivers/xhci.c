/*
 * xhci.c — xHCI (USB 3.0) host controller driver
 *
 * PCI-based USB 3.0 eXtensible Host Controller Interface driver.
 * Probes via class code 0x0C, 0x03, 0x30.
 */

#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static struct xhci_controller g_xhci;
static int g_xhci_init_done = 0;

/* Probe for xHCI controller via PCI */
static int xhci_probe_pci(void) {
    struct pci_device pci;
    int ret = pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS, &pci);
    if (ret < 0)
        return -1;

    /* Must be prog_if 0x30 for xHCI */
    if ((pci.class_code != XHCI_PCI_CLASS) || (pci.subclass != XHCI_PCI_SUBCLASS))
        return -1;

    pci_enable_bus_master(&pci);

    /* BAR0 contains MMIO registers */
    uint32_t bar0 = pci.bar[0];
    if (bar0 & 1) {
        kprintf("[xHCI] BAR0 is I/O space, not supported\n");
        return -1;
    }

    uint64_t mmio_base = (bar0 & 0xFFFFFFF0);
    if (mmio_base == 0)
        return -1;

    uint64_t virt_base = (uint64_t)PHYS_TO_VIRT((void*)(uintptr_t)mmio_base);

    g_xhci.cap_regs = virt_base;
    g_xhci.irq = pci.irq;

    /* Read capability registers */
    uint8_t caplength = xhci_read8(&g_xhci, g_xhci.cap_regs, 0x00);
    uint32_t hcsparams1 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_HCCPARAMS1);

    g_xhci.op_regs = virt_base + caplength;
    g_xhci.max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    g_xhci.max_slots = (uint8_t)(hcsparams2 & 0x1F);
    g_xhci.db_off = (hccparams1 & 0xFFFF);
    g_xhci.rt_off = xhci_read32(&g_xhci, g_xhci.cap_regs, XHCI_CAP_RTSOFF);

    if (g_xhci.max_ports > XHCI_MAX_PORTS)
        g_xhci.max_ports = XHCI_MAX_PORTS;

    kprintf("[xHCI] Found: VID=0x%04X DID=0x%04X, Ports=%d, Slots=%d\n",
            pci.vendor_id, pci.device_id, g_xhci.max_ports, g_xhci.max_slots);

    g_xhci.present = 1;
    return 0;
}

/* Reset and start xHCI controller */
static int xhci_start_controller(void) {
    /* Read USBSTS */
    uint32_t sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);

    /* Reset controller */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD, XHCI_CMD_HCRST);

    /* Wait for reset to complete */
    int timeout = 100000;
    while (timeout--) {
        sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);
        if (xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD) & XHCI_CMD_HCRST) {
            __asm__ volatile("pause");
        } else {
            break;
        }
    }

    if (timeout <= 0) {
        kprintf("[xHCI] Controller reset timeout\n");
        return -1;
    }

    /* Set max slots in CONFIG register */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_CONFIG, g_xhci.max_slots);

    /* Start the controller */
    xhci_write32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);

    /* Wait for HCHalted bit to clear */
    timeout = 100000;
    while (timeout--) {
        sts = xhci_read32(&g_xhci, g_xhci.op_regs, XHCI_OP_USBSTS);
        if (!(sts & XHCI_STS_HCH))
            break;
        __asm__ volatile("pause");
    }

    if (timeout <= 0) {
        kprintf("[xHCI] Controller start timeout\n");
        return -1;
    }

    kprintf("[xHCI] Controller started\n");
    return 0;
}

int xhci_port_reset(int port) {
    if (!g_xhci.present || port < 0 || port >= g_xhci.max_ports)
        return -1;

    uint64_t portsc_base = g_xhci.op_regs + XHCI_PORTSC + (port * 0x10);
    uint32_t portsc = xhci_read32(&g_xhci, portsc_base, 0);

    /* Set port reset bit */
    xhci_write32(&g_xhci, portsc_base, 0, portsc | XHCI_PORTSC_PR);

    /* Wait for reset to complete */
    int timeout = 100000;
    while (timeout--) {
        portsc = xhci_read32(&g_xhci, portsc_base, 0);
        if (!(portsc & XHCI_PORTSC_PR))
            break;
        __asm__ volatile("pause");
    }

    if (timeout <= 0)
        return -1;

    /* Acknowledge port reset change */
    xhci_write32(&g_xhci, portsc_base, 0, portsc | XHCI_PORTSC_PRC);
    return 0;
}

int xhci_port_status(int port) {
    if (!g_xhci.present || port < 0 || port >= g_xhci.max_ports)
        return -1;

    uint64_t portsc_base = g_xhci.op_regs + XHCI_PORTSC + (port * 0x10);
    uint32_t portsc = xhci_read32(&g_xhci, portsc_base, 0);

    return (int)portsc;
}

int xhci_init(void) {
    if (g_xhci_init_done)
        return 0;

    memset(&g_xhci, 0, sizeof(g_xhci));

    if (xhci_probe_pci() < 0) {
        kprintf("[xHCI] No xHCI controller found\n");
        g_xhci_init_done = 1;
        return -1;
    }

    if (xhci_start_controller() < 0) {
        kprintf("[xHCI] Failed to start controller\n");
        return -1;
    }

    /* Reset all ports */
    for (int i = 0; i < g_xhci.max_ports; i++) {
        if (xhci_port_status(i) & XHCI_PORTSC_CCS) {
            kprintf("[xHCI] Port %d: device connected\n", i);
        }
    }

    g_xhci_init_done = 1;
    kprintf("[xHCI] Driver initialized\n");
    return 0;
}

int xhci_is_present(void) {
    return g_xhci.present;
}

void xhci_print_info(void) {
    if (!g_xhci.present) {
        kprintf("xHCI: Not present\n");
        return;
    }
    kprintf("xHCI: present, %d ports, %d slots\n",
            g_xhci.max_ports, g_xhci.max_slots);
}
#include "module.h"
module_init(xhci_init);

/* ── Stub: xhci_reset ─────────────────────────────── */
int xhci_reset(void *dev)
{
    (void)dev;
    kprintf("[xhci] xhci_reset: not yet implemented\n");
    return 0;
}
/* ── Stub: xhci_submit_urb ─────────────────────────────── */
int xhci_submit_urb(void *urb)
{
    (void)urb;
    kprintf("[xhci] xhci_submit_urb: not yet implemented\n");
    return 0;
}
void xhci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    /* xHCI IRQ handler depends on types not yet defined */
}

