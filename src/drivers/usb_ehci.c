/*
 * USB EHCI Host Controller Driver
 *
 * Supports USB 2.0 (EHCI) controllers.  Handles controller detection,
 * reset, port power-on, and basic high-speed device detection.
 *
 * Full HID/mass-storage enumeration is layered on top; this file provides
 * the EHCI initialisation and port scanning so that devices show up in
 * `lsusb` on real hardware (e.g. ThinkPad X220).
 *
 * References:
 *   Enhanced Host Controller Interface Specification for Universal Serial Bus
 *   Revision 1.0, March 12, 2002.
 */
#include "usb.h"
#include "pci.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"

/* ── EHCI Capability Register offsets ──────────────────────────────────────── */
#define EHCI_CAPLENGTH   0x00   /* Capability registers length (1 byte) */
#define EHCI_HCIVERSION  0x02   /* HC Interface Version */
#define EHCI_HCSPARAMS   0x04   /* Structural Parameters */
#define EHCI_HCCPARAMS   0x08   /* Capability Parameters */

/* ── EHCI Operational Register offsets (relative to op_base = cap_base+caplength) */
#define EHCI_USBCMD      0x00   /* USB Command */
#define EHCI_USBSTS      0x04   /* USB Status */
#define EHCI_USBINTR     0x08   /* USB Interrupt Enable */
#define EHCI_FRINDEX     0x0C   /* USB Frame Index */
#define EHCI_CTDSEGMENT  0x10   /* 4G Segment Selector */
#define EHCI_PERIODICBASE 0x14  /* Periodic Frame List Base Address */
#define EHCI_ASYNCLISTADDR 0x18 /* Async List Address */
#define EHCI_CONFIGFLAG  0x40   /* Configure Flag */
#define EHCI_PORTSC_BASE 0x44   /* Port Status/Control registers (one per port) */

/* USBCMD bits */
#define EHCI_CMD_RUN     (1u << 0)
#define EHCI_CMD_HCRESET (1u << 1)
#define EHCI_CMD_PSE     (1u << 4)   /* Periodic Schedule Enable */
#define EHCI_CMD_ASE     (1u << 5)   /* Async Schedule Enable */
#define EHCI_CMD_ASPME   (1u << 11)  /* Async Schedule Park Mode Enable */

/* USBSTS bits */
#define EHCI_STS_HALTED  (1u << 12)
#define EHCI_STS_PSS     (1u << 14)  /* Periodic Schedule Status */
#define EHCI_STS_ASS     (1u << 15)  /* Async Schedule Status */

/* PORTSC bits */
#define PORTSC_CCS       (1u << 0)   /* Current Connect Status */
#define PORTSC_CSC       (1u << 1)   /* Connect Status Change */
#define PORTSC_PED       (1u << 2)   /* Port Enabled/Disabled */
#define PORTSC_PEDC      (1u << 3)   /* Port Enable/Disable Change */
#define PORTSC_PR        (1u << 8)   /* Port Reset */
#define PORTSC_PP        (1u << 12)  /* Port Power */
#define PORTSC_LS_MASK   (3u << 10)  /* Line Status */
#define PORTSC_LS_K      (1u << 10)  /* K-state → low-speed */
#define PORTSC_OWNER     (1u << 13)  /* Port Owner (1=companion OHCI/UHCI) */
#define PORTSC_SPEED_MASK (3u << 20) /* Port Speed (xHCI ext) */

/* ── Driver state ──────────────────────────────────────────────────────────── */
#define EHCI_MAX_CONTROLLERS 4

static struct {
    uint64_t cap_base;   /* capability register MMIO base */
    uint64_t op_base;    /* operational register MMIO base */
    int      n_ports;
} ehci_ctrl[EHCI_MAX_CONTROLLERS];

static int ehci_count = 0;

static struct usb_device usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;

/* ── MMIO helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t cap_read(int c, uint32_t off) {
    return *(volatile uint32_t *)(ehci_ctrl[c].cap_base + off);
}
static inline uint32_t op_read(int c, uint32_t off) {
    return *(volatile uint32_t *)(ehci_ctrl[c].op_base + off);
}
static inline void op_write(int c, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(ehci_ctrl[c].op_base + off) = val;
}

static void busy_wait_n(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── Initialise one EHCI controller ───────────────────────────────────────── */
static int ehci_init_controller(uint8_t bus, uint8_t slot, uint32_t bar0) {
    if (ehci_count >= EHCI_MAX_CONTROLLERS) return -1;

    uint64_t cap = (uint64_t)(bar0 & ~0x1Fu);
    if (cap == 0) return -2;

    int c = ehci_count;
    ehci_ctrl[c].cap_base = cap;

    /* caplength: offset of operational registers from cap_base */
    uint8_t caplength = *(volatile uint8_t *)cap;
    ehci_ctrl[c].op_base = cap + caplength;

    uint32_t hcsparams = cap_read(c, EHCI_HCSPARAMS);
    int n_ports = (int)(hcsparams & 0x0F);
    ehci_ctrl[c].n_ports = n_ports;

    /* Enable bus master */
    {
        struct pci_device dev;
        dev.bus = bus; dev.slot = slot; dev.func = 0;
        pci_enable_bus_master(&dev);
    }

    /* Route all ports from companion controllers to EHCI */
    uint32_t eecp = (cap_read(c, EHCI_HCCPARAMS) >> 8) & 0xFF;
    if (eecp >= 0x40) {
        /* Clear BIOS ownership (OS semaphore handoff) */
        uint32_t legsup = pci_read(bus, slot, 0, eecp);
        if (legsup & (1u << 16)) {
            /* Set OS owned semaphore */
            pci_write(bus, slot, 0, eecp, legsup | (1u << 24));
            int timeout = 100000;
            while ((pci_read(bus, slot, 0, eecp) & (1u << 16)) && --timeout)
                busy_wait_n(10);
        }
    }

    /* Reset the host controller */
    uint32_t cmd = op_read(c, EHCI_USBCMD);
    cmd &= ~EHCI_CMD_RUN;
    op_write(c, EHCI_USBCMD, cmd);
    /* Wait for HALTED */
    int timeout = 50000;
    while (!(op_read(c, EHCI_USBSTS) & EHCI_STS_HALTED) && --timeout)
        busy_wait_n(10);

    op_write(c, EHCI_USBCMD, EHCI_CMD_HCRESET);
    timeout = 50000;
    while ((op_read(c, EHCI_USBCMD) & EHCI_CMD_HCRESET) && --timeout)
        busy_wait_n(10);
    if (!timeout) return -3;  /* reset timed out */

    /* Disable all interrupts */
    op_write(c, EHCI_USBINTR, 0);
    op_write(c, EHCI_USBSTS,  0x3F);  /* clear all status bits */

    /* Set segment selector to 0 (first 4 GB) */
    op_write(c, EHCI_CTDSEGMENT, 0);

    /* Start the controller */
    op_write(c, EHCI_USBCMD, EHCI_CMD_RUN);
    timeout = 50000;
    while ((op_read(c, EHCI_USBSTS) & EHCI_STS_HALTED) && --timeout)
        busy_wait_n(10);

    /* Route all ports to this EHCI (not companion) */
    op_write(c, EHCI_CONFIGFLAG, 1);

    /* Power up all ports and scan */
    for (int p = 0; p < n_ports && usb_device_count < USB_MAX_DEVICES; p++) {
        uint32_t portsc = op_read(c, EHCI_PORTSC_BASE + p * 4);

        /* Power on if port power control is supported */
        if (!(portsc & PORTSC_PP)) {
            op_write(c, EHCI_PORTSC_BASE + p * 4, portsc | PORTSC_PP);
            busy_wait_n(200000);  /* 20 ms debounce */
            portsc = op_read(c, EHCI_PORTSC_BASE + p * 4);
        }

        /* Clear CSC */
        if (portsc & PORTSC_CSC) {
            op_write(c, EHCI_PORTSC_BASE + p * 4, portsc | PORTSC_CSC);
            busy_wait_n(10000);
            portsc = op_read(c, EHCI_PORTSC_BASE + p * 4);
        }

        if (!(portsc & PORTSC_CCS)) continue;  /* no device */

        /* If line status == K-state, device is low-speed → hand to companion */
        if ((portsc & PORTSC_LS_MASK) == PORTSC_LS_K) {
            op_write(c, EHCI_PORTSC_BASE + p * 4, portsc | PORTSC_OWNER);
            continue;
        }

        /* Reset port to enable it */
        portsc &= ~PORTSC_PED;
        op_write(c, EHCI_PORTSC_BASE + p * 4, (portsc & ~PORTSC_PR) | PORTSC_PR);
        busy_wait_n(500000);   /* 50 ms reset */
        op_write(c, EHCI_PORTSC_BASE + p * 4, portsc & ~PORTSC_PR);
        busy_wait_n(50000);    /* 5 ms recovery */

        portsc = op_read(c, EHCI_PORTSC_BASE + p * 4);
        if (!(portsc & PORTSC_PED)) continue;  /* not a high-speed device */

        /* Device detected and enabled — record it */
        struct usb_device *dev = &usb_devices[usb_device_count++];
        memset(dev, 0, sizeof(*dev));
        dev->addr  = (uint8_t)(usb_device_count);
        dev->speed = 2; /* high-speed */
        /* Full descriptor read requires TD scheduling; mark as generic for now */
        dev->class_code = 0xFF;
    }

    ehci_count++;
    kprintf("  EHCI controller %d: %d ports, %d devices\n",
            (uint64_t)c, (uint64_t)n_ports,
            (uint64_t)usb_device_count);
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────────────── */
int usb_is_present(void)    { return ehci_count > 0; }
int usb_get_device_count(void) { return usb_device_count; }
struct usb_device *usb_get_device(int idx) {
    if (idx < 0 || idx >= usb_device_count) return (void *)0;
    return &usb_devices[idx];
}

/* Expose the first controller's operational base for the MSC transfer engine */
uint64_t ehci_get_op_base(void) {
    if (ehci_count == 0) return 0;
    return ehci_ctrl[0].op_base;
}
int ehci_get_n_ports(void) {
    if (ehci_count == 0) return 0;
    return ehci_ctrl[0].n_ports;
}

int usb_init(void) {
    /* Scan PCI for EHCI controllers (class 0x0C, subclass 0x03, prog-if 0x20) */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t r0 = pci_read(bus, slot, 0, 0);
            if ((r0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t r2  = pci_read(bus, slot, 0, 0x08);
            uint8_t  cls = (r2 >> 24) & 0xFF;
            uint8_t  sub = (r2 >> 16) & 0xFF;
            uint8_t  prg = (r2 >> 8)  & 0xFF;
            if (cls == 0x0C && sub == 0x03 && prg == 0x20) {
                uint32_t bar0 = pci_read(bus, slot, 0, 0x10);
                ehci_init_controller((uint8_t)bus, (uint8_t)slot, bar0);
            }
        }
    }
    return ehci_count > 0 ? 0 : -1;
}
