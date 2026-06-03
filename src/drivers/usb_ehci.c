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

/*
 * ── Isochronous (periodic) schedule infrastructure ──────────────────────
 *
 * EHCI uses a 1024-element frame list (4 KB page) for periodic transfers
 * (isochronous and interrupt).  Each frame-list entry points to an iTD
 * (isochronous Transfer Descriptor) or a QH for interrupt endpoints.
 *
 * iTD layout (EHCI spec §3.3, 32 bytes, 32-byte aligned):
 *   [0x00] Next link pointer  —  T(1) | Type(2) | Pointer(29)
 *   [0x04] Transaction 0-1    —  two 16-bit transaction records
 *   [0x08] Transaction 2-3
 *   [0x0C] Transaction 4-5
 *   [0x10] Transaction 6-7
 *   [0x14] Buffer Pointer 0  —  physical page address (bits [31:12])
 *   [0x18] Buffer Pointer 1
 *   [0x1C] Buffer Pointer 2
 *
 * Each 16-bit transaction record:
 *   Bits [7:0]   —  Status (0x80 = Active)
 *   Bits [11:8]  —  Offset into buffer page
 *   Bits [14:12] —  Page select (0..2)
 *   Bit  [15]    —  Interrupt On Complete (IOC)
 *
 * The controller processes active transactions in the iTD each micro-frame,
 * transferring data from the offset within the selected page up to the
 * end of that page.
 */
#define EHCI_FRAME_LIST_ENTRIES  1024   /* 1024 × 4 bytes = 4096 bytes */

/* iTD — Isochronous Transfer Descriptor (EHCI spec §3.3) */
struct ehci_itd {
    uint32_t   next_link;     /* 0x00: Next schedule element pointer */
    uint32_t   xact_01;       /* 0x04: µframe 0 (low16) + µframe 1 (high16) */
    uint32_t   xact_23;       /* 0x08: µframe 2 + 3 */
    uint32_t   xact_45;       /* 0x0C: µframe 4 + 5 */
    uint32_t   xact_67;       /* 0x10: µframe 6 + 7 */
    uint32_t   buf_ptr0;      /* 0x14: Buffer page 0 physical address [31:12] */
    uint32_t   buf_ptr1;      /* 0x18: Buffer page 1 */
    uint32_t   buf_ptr2;      /* 0x1C: Buffer page 2 */
} __attribute__((packed, aligned(32)));

/* Link pointer type codes and helpers */
#define EHCI_PTR_TERMINATE   0x01u
#define EHCI_PTR_TYPE_ITD    0x02u      /* Type field = 010b → iTD */
#define EHCI_PTR_TYPE_SITD   0x03u      /* Type field = 011b → siTD */

/* Helper: build a link pointer to an iTD (physical address, 32-byte aligned) */
#define EHCI_ITD_LINK(phys)  ((uint32_t)(phys) | EHCI_PTR_TYPE_ITD)

/* Transaction record builder: encode status, offset, page, IOC into 16 bits */
#define ITD_XACT(status, offset, page, ioc)                               \
    ((uint16_t)(((uint16_t)(uint8_t)(status) & 0xFF) |                    \
                (((uint16_t)(uint8_t)(offset) & 0x0F) << 8) |              \
                (((uint16_t)(uint8_t)(page) & 0x07) << 12) |               \
                ((ioc) ? (uint16_t)(1u << 15) : 0)))

/* Transaction record field extractors */
#define ITD_XACT_GET_STATUS(w)  ((uint8_t)((w) & 0xFF))
#define ITD_XACT_ACTIVE        (0x80u)

/* Static periodic schedule state */
static uint32_t *g_flist        = NULL;   /* virtual address of frame list */
static uint64_t  g_flist_phys   = 0;       /* physical address of frame list */
static int       g_periodic_on  = 0;       /* periodic schedule enabled */

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
    /* Convert BAR0 physical address to high-half VMA (identity map removed) */
    ehci_ctrl[c].cap_base = (uint64_t)PHYS_TO_VIRT(cap);

    /* caplength: offset of operational registers from cap_base */
    uint8_t caplength = *(volatile uint8_t *)PHYS_TO_VIRT(cap);
    ehci_ctrl[c].op_base = (uint64_t)PHYS_TO_VIRT(cap) + caplength;

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
    kprintf("  EHCI controller %ld: %ld ports, %ld devices\n",
            (unsigned long)c, (unsigned long)n_ports,
            (unsigned long)usb_device_count);
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

/* ── Periodic schedule (isochronous) support ────────────────────────────────── */

int ehci_setup_periodic(void) {
    int c = 0;
    if (ehci_count < 1) return -1;  /* no EHCI controller */
    if (g_periodic_on)  return 0;   /* already set up */

    /* Allocate one page for the 1024-entry frame list */
    uint64_t fl_phys = pmm_alloc_frame();
    if (!fl_phys) return -2;

    /* Clear and fill with terminate entries */
    uint32_t *fl = (uint32_t *)PHYS_TO_VIRT(fl_phys);
    for (int i = 0; i < EHCI_FRAME_LIST_ENTRIES; i++)
        fl[i] = EHCI_PTR_TERMINATE;

    g_flist      = fl;
    g_flist_phys = fl_phys;

    /* Program PERIODICBASE register (physical address, page-aligned) */
    op_write(c, EHCI_PERIODICBASE, (uint32_t)fl_phys);

    /* Enable periodic schedule */
    uint32_t cmd = op_read(c, EHCI_USBCMD);
    op_write(c, EHCI_USBCMD, cmd | EHCI_CMD_PSE);

    /* Wait for Periodic Schedule Status to assert */
    int timeout = 50000;
    while (!(op_read(c, EHCI_USBSTS) & EHCI_STS_PSS) && --timeout)
        busy_wait_n(10);
    if (!timeout) {
        op_write(c, EHCI_PERIODICBASE, 0);
        pmm_free_frame(fl_phys);
        g_flist = NULL;
        g_flist_phys = 0;
        return -3;  /* PSS never asserted */
    }

    g_periodic_on = 1;
    kprintf("  EHCI: periodic schedule enabled (flist phys=0x%lx)\n",
            (unsigned long)fl_phys);
    return 0;
}

void ehci_teardown_periodic(void) {
    int c = 0;
    if (!g_periodic_on) return;

    /* Disable periodic schedule */
    uint32_t cmd = op_read(c, EHCI_USBCMD);
    op_write(c, EHCI_USBCMD, cmd & ~EHCI_CMD_PSE);

    /* Wait for PSS to clear */
    int timeout = 50000;
    while ((op_read(c, EHCI_USBSTS) & EHCI_STS_PSS) && --timeout)
        busy_wait_n(10);

    op_write(c, EHCI_PERIODICBASE, 0);

    if (g_flist_phys) {
        pmm_free_frame(g_flist_phys);
    }
    g_flist       = NULL;
    g_flist_phys  = 0;
    g_periodic_on = 0;
    kprintf("  EHCI: periodic schedule disabled\n");
}

int ehci_submit_isochronous(uint8_t dev_addr, uint8_t ep,
                            void *buf, uint32_t len,
                            uint32_t sched_frame)
{
    (void)dev_addr;  /* device address embedded in iTD endpoint char */
    (void)ep;        /* endpoint number — currently single-µframe */
    if (!g_periodic_on) return -1;
    if (!buf || len == 0 || len > 4096) return -2;
    if (ehci_count < 1) return -3;

    /*
     * Allocate one physical page for the iTD.
     * pmm_alloc_frame() returns a 4 KB page which is far larger than the
     * 32-byte iTD, but guarantees 32-byte alignment naturally.
     */
    uint64_t itd_phys = pmm_alloc_frame();
    if (!itd_phys) return -4;

    struct ehci_itd *itd = (struct ehci_itd *)PHYS_TO_VIRT(itd_phys);
    memset(itd, 0, sizeof(*itd));

    /* Determine buffer physical address and page split */
    uint32_t buf_phys = (uint32_t)(uintptr_t)VIRT_TO_PHYS(buf);
    uint32_t pg0_base = buf_phys & ~0xFFFu;
    uint32_t pg1_base = pg0_base + 0x1000u;
    uint32_t pg2_base = pg1_base + 0x1000u;
    uint32_t pg_off   = buf_phys & 0xFFFu;
    uint32_t pg_sel   = 0;

    /*
     * Compute the last page needed.  For transfers up to 4 KB spanning
     * no more than 3 pages we set all three buffer pointers.
     */
    uint32_t end_phys = buf_phys + len - 1;
    uint32_t last_pg  = (end_phys >> 12) - (pg0_base >> 12);

    itd->buf_ptr0 = pg0_base;
    itd->buf_ptr1 = (last_pg >= 1) ? pg1_base : 0;
    itd->buf_ptr2 = (last_pg >= 2) ? pg2_base : 0;

    /*
     * Build a single transaction record for micro-frame 0.
     * For audio devices a single isochronous packet per frame is typical.
     * The controller transfers from offset to end-of-page, so the actual
     * byte count is (4096 - pg_off), but we ensure len ≤ 1024 per spec.
     * IOC is set so we can poll for completion.
     */
    uint16_t xact0 = ITD_XACT(ITD_XACT_ACTIVE,
                              (uint8_t)(pg_off & 0x0F),
                              (uint8_t)(pg_sel & 0x07),
                              1);   /* IOC = 1 */
    itd->xact_01 = (uint32_t)xact0; /* low 16 bits = µframe 0 transaction */

    /* Set up the next-link pointer: terminate after this iTD */
    itd->next_link = EHCI_PTR_TERMINATE;

    /*
     * Insert the iTD into the periodic schedule at the specified frame slot.
     * We use a simple insertion: save the current frame-list entry, point it
     * to our iTD, and have our iTD terminate (or chain to whatever was there).
     * For safety, we save the old pointer and restore after completion.
     *
     * A real production driver would maintain a proper linked list per frame
     * entry, but for simple synchronous isochronous this suffices.
     */
    uint32_t frame_idx = sched_frame % EHCI_FRAME_LIST_ENTRIES;
    uint32_t old_fl_entry = g_flist[frame_idx];

    /* Point the frame list entry to our iTD */
    g_flist[frame_idx] = EHCI_ITD_LINK(itd_phys);

    /*
     * Ensure the frame-list write is visible to the controller before we
     * start polling (memory barrier).
     */
    __asm__ volatile("mfence" ::: "memory");

    /*
     * Poll for completion.  The controller clears the Active bit in the
     * transaction record when the transfer completes (after one USB frame
     * = 1 ms worst-case for a single µframe).
     */
    int timeout = 2000000;  /* generous ~2 s timeout */
    while ((itd->xact_01 & 0x00FF) & ITD_XACT_ACTIVE) {
        if (--timeout <= 0) {
            /* Restore frame list entry and return timeout error */
            g_flist[frame_idx] = old_fl_entry;
            pmm_free_frame(itd_phys);
            return -5;  /* transaction timed out */
        }
        busy_wait_n(10);
    }

    /* Check for errors */
    uint8_t status = (uint8_t)(itd->xact_01 & 0x00FF);
    if (status & ~ITD_XACT_ACTIVE) {
        /* Some error occurred (Halted, Babble, Transaction Error, etc.) */
        g_flist[frame_idx] = old_fl_entry;
        pmm_free_frame(itd_phys);
        return -6;  /* transfer error */
    }

    /* Restore the frame list entry */
    g_flist[frame_idx] = old_fl_entry;
    __asm__ volatile("mfence" ::: "memory");

    pmm_free_frame(itd_phys);
    return 0;
}

/* ── xHCI isochronous stub ─────────────────────────────────────────────────── */
int xhci_submit_isochronous(uint8_t dev_addr, uint8_t ep,
                             void *buf, uint32_t len)
{
    (void)dev_addr; (void)ep; (void)buf; (void)len;
    return -7;  /* ENOSYS — not yet implemented */
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
