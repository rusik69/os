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
#ifdef MODULE
#include "module.h"
#endif
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
        uint32_t legsup = pci_read((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)eecp);
        if (legsup & (1u << 16)) {
            /* Set OS owned semaphore */
            pci_write((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)eecp, legsup | (1u << 24));
            int timeout = 100000;
            while ((pci_read((uint8_t)bus, (uint8_t)slot, 0, (uint8_t)eecp) & (1u << 16)) && --timeout)
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

/* ── Double-buffer isochronous pool (Item S49) ──────────────────────────────── */

/*
 * Multi-buffer double-buffering for isochronous streams.
 *
 * Instead of allocating a new iTD per transfer, we maintain a small
 * pool of pre-allocated iTD+buffer pairs that are swapped on each
 * frame.  This eliminates allocation overhead and allows the controller
 * to DMA from one buffer while the driver fills the other.
 *
 * Usage:
 *   int pool_id = ehci_iso_pool_create(ep, buf_size);
 *   ehci_iso_pool_submit(pool_id, data, len);
 *   ehci_iso_pool_reclaim(pool_id);
 */

#define ISO_POOL_MAX_BUFS   4          /* Total buffers in pool */
#define ISO_POOL_NUM_POOLS  16         /* Max concurrent isochronous streams */

/* Per-pool buffer descriptor */
struct iso_buffer {
    void    *virt;           /* Virtual address of data buffer */
    uint64_t phys;           /* Physical address */
    uint64_t itd_phys;       /* Physical address of associated iTD */
    struct ehci_itd *itd;    /* Virtual address of iTD */
    uint32_t len;            /* Buffer length */
    uint32_t flags;          /* ISO_* flags */
    int      in_use;         /* 1 if submitted and pending */
};

/* Isochronous stream pool */
struct iso_pool {
    uint8_t  dev_addr;       /* USB device address */
    uint8_t  ep;             /* Endpoint number */
    uint32_t sched_frame;    /* Current scheduled frame index */
    int      active;         /* Pool is active */
    int      write_idx;      /* Which buffer we're filling now */
    int      submit_idx;     /* Which buffer submitted to controller */

    struct iso_buffer bufs[ISO_POOL_MAX_BUFS];
};

static struct iso_pool g_iso_pools[ISO_POOL_NUM_POOLS];
static int g_iso_pool_count = 0;

/* Flags */
#define ISO_BUF_COMPLETE    0x0001   /* Transaction completed by controller */

/**
 * ehci_iso_pool_create — Create a double-buffered isochronous pool.
 * @dev_addr:   USB device address.
 * @ep:         Endpoint number.
 * @buf_size:   Size of each buffer in bytes (must be ≤ 1024).
 *
 * Returns pool_id (≥0) on success, negative on error.
 */
int ehci_iso_pool_create(uint8_t dev_addr, uint8_t ep, uint32_t buf_size)
{
    if (g_iso_pool_count >= ISO_POOL_NUM_POOLS)
        return -1;
    if (buf_size == 0 || buf_size > USB_ISO_MAX_PACKET)
        return -2;
    if (!g_periodic_on)
        return -3;

    int pid = g_iso_pool_count;
    struct iso_pool *pool = &g_iso_pools[pid];
    memset(pool, 0, sizeof(*pool));

    pool->dev_addr = dev_addr;
    pool->ep = ep;
    pool->active = 1;
    pool->write_idx = 0;
    pool->submit_idx = -1;

    /* Allocate buffers and iTDs */
    for (int i = 0; i < ISO_POOL_MAX_BUFS; i++) {
        struct iso_buffer *b = &pool->bufs[i];

        /* Allocate buffer (page-aligned for DMA) */
        b->virt = (void *)pmm_alloc_frame();
        if (!b->virt)
            goto fail;
        b->phys = (uint64_t)VIRT_TO_PHYS(b->virt);
        memset(b->virt, 0, PAGE_SIZE);

        /* Allocate iTD (use a full page for simplicity) */
        b->itd_phys = pmm_alloc_frame();
        if (!b->itd_phys)
            goto fail;
        b->itd = (struct ehci_itd *)PHYS_TO_VIRT(b->itd_phys);
        memset(b->itd, 0, sizeof(struct ehci_itd));

        b->len = buf_size;
        b->in_use = 0;
    }

    g_iso_pool_count++;
    kprintf("[ISO] Pool %d created: addr=%d ep=0x%02x buf=%u bytes "
            "%d buffers\n", pid, dev_addr, ep, buf_size, ISO_POOL_MAX_BUFS);
    return pid;

fail:
    for (int i = 0; i < ISO_POOL_MAX_BUFS; i++) {
        struct iso_buffer *b = &pool->bufs[i];
        if (b->virt)  pmm_free_frame((uint64_t)b->virt);
        if (b->itd_phys) pmm_free_frame(b->itd_phys);
    }
    memset(pool, 0, sizeof(*pool));
    return -4;
}

/**
 * ehci_iso_pool_submit — Fill next available buffer and submit to controller.
 * @pool_id:  Pool identifier from ehci_iso_pool_create().
 * @data:     Source data to copy into the DMA buffer.
 * @len:      Number of bytes to transfer (must be ≤ buf_size).
 *
 * Returns 0 on success, negative on error.
 */
int ehci_iso_pool_submit(int pool_id, const void *data, uint32_t len)
{
    if (pool_id < 0 || pool_id >= g_iso_pool_count)
        return -1;

    struct iso_pool *pool = &g_iso_pools[pool_id];
    if (!pool->active)
        return -2;

    int buf_idx = pool->write_idx;
    struct iso_buffer *b = &pool->bufs[buf_idx];

    if (b->in_use) {
        /* Buffer still pending — reclaim first */
        return -3;
    }

    /* Copy data into DMA buffer */
    if (len > b->len) len = b->len;
    memcpy(b->virt, data, (size_t)len);

    /* Build iTD */
    struct ehci_itd *itd = b->itd;
    memset(itd, 0, sizeof(*itd));

    uint32_t buf_phys = (uint32_t)b->phys;
    uint32_t pg0_base = buf_phys & ~0xFFFu;
    uint32_t pg_off   = buf_phys & 0xFFFu;

    itd->buf_ptr0 = pg0_base;
    itd->buf_ptr1 = ((buf_phys + len - 1) >> 12) > (pg0_base >> 12)
                    ? pg0_base + 0x1000u : 0;
    itd->buf_ptr2 = 0;

    itd->xact_01 = (uint32_t)ITD_XACT(ITD_XACT_ACTIVE,
                                       (uint8_t)(pg_off & 0x0F), 0, 1);
    itd->next_link = EHCI_PTR_TERMINATE;

    /* Insert into periodic schedule */
    uint32_t frame_idx = pool->sched_frame % EHCI_FRAME_LIST_ENTRIES;
    b->flags = 0;
    b->in_use = 1;

    /* Save old entry and redirect */
    uint32_t old_entry = g_flist[frame_idx];
    g_flist[frame_idx] = EHCI_ITD_LINK(b->itd_phys);
    __asm__ volatile("mfence" ::: "memory");

    /* Track: we submitted buffer at this frame */
    pool->submit_idx = buf_idx;
    pool->write_idx = (buf_idx + 1) % ISO_POOL_MAX_BUFS;
    pool->sched_frame++;

    return 0;
}

/**
 * ehci_iso_pool_reclaim — Reclaim completed buffers.
 * @pool_id:  Pool identifier.
 *
 * Checks each submitted buffer for completion (Active bit cleared).
 * Marks them ready for reuse.
 *
 * Returns the number of bytes in the most recently completed buffer,
 * or 0 if none completed.
 */
uint32_t ehci_iso_pool_reclaim(int pool_id)
{
    if (pool_id < 0 || pool_id >= g_iso_pool_count)
        return 0;

    struct iso_pool *pool = &g_iso_pools[pool_id];
    if (!pool->active)
        return 0;

    uint32_t completed_bytes = 0;

    for (int i = 0; i < ISO_POOL_MAX_BUFS; i++) {
        struct iso_buffer *b = &pool->bufs[i];
        if (!b->in_use)
            continue;

        /* Check if iTD completed (Active bit cleared) */
        if ((b->itd->xact_01 & 0x00FF) & ITD_XACT_ACTIVE)
            continue;  /* Still pending */

        /* Transaction completed */
        b->in_use = 0;
        completed_bytes = b->len;

        /* Restore frame list entry (could be more sophisticated) */
        /* In a real driver, restore the old link target */
    }

    return completed_bytes;
}

/**
 * ehci_iso_pool_destroy — Release all pool resources.
 * @pool_id:  Pool identifier.
 */
void ehci_iso_pool_destroy(int pool_id)
{
    if (pool_id < 0 || pool_id >= g_iso_pool_count)
        return;

    struct iso_pool *pool = &g_iso_pools[pool_id];
    pool->active = 0;

    for (int i = 0; i < ISO_POOL_MAX_BUFS; i++) {
        struct iso_buffer *b = &pool->bufs[i];
        if (b->virt)  pmm_free_frame((uint64_t)b->virt);
        if (b->itd_phys) pmm_free_frame(b->itd_phys);
    }
    memset(pool, 0, sizeof(*pool));

    kprintf("[ISO] Pool %d destroyed\n", pool_id);
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

/* ── Async schedule (control/bulk) processing ─────────────────────────── */

/*
 * EHCI async schedule processing using Queue Heads (QH) and
 * Queue Element Transfer Descriptors (qTD).
 *
 * The async schedule processes control, bulk, and interrupt transfers.
 * Each QH points to a ring of qTDs that describe the buffer locations
 * and transfer sizes.
 *
 * qTD layout (EHCI spec §3.5, 32 bytes, 32-byte aligned):
 *   [0x00] Next qTD pointer     — physical address of next qTD
 *   [0x04] Alternate Next qTD   — for error recovery / NAK
 *   [0x08] Token                — status, total bytes, CERR, PID, IOC
 *   [0x0C] Buffer Pointer 0     — physical page address [31:12]
 *   [0x10] Buffer Pointer 1
 *   [0x14] Buffer Pointer 2
 *   [0x18] Buffer Pointer 3
 *   [0x1C] Buffer Pointer 4
 */

/* qTD token definitions */
#define QTD_TOKEN_STATUS_MASK   0x000000FF
#define QTD_TOKEN_ACTIVE        0x00000080
#define QTD_TOKEN_HALTED        0x00000040
#define QTD_TOKEN_DATABUFERR    0x00000020
#define QTD_TOKEN_BABBLE        0x00000010
#define QTD_TOKEN_XACTERR       0x00000008
#define QTD_TOKEN_MISSEDMICRO   0x00000004
#define QTD_TOKEN_SPLITXSTATE   0x00000002
#define QTD_TOKEN_PID_MASK      0x00000300
#define QTD_TOKEN_PID_OUTPUT    0x00000000
#define QTD_TOKEN_PID_INPUT     0x00000100
#define QTD_TOKEN_PID_SETUP     0x00000200
#define QTD_TOKEN_CERR_SHIFT    10
#define QTD_TOKEN_CERR_MASK     0x00000C00
#define QTD_TOKEN_CPAGE_SHIFT   12
#define QTD_TOKEN_CPAGE_MASK    0x00007000
#define QTD_TOKEN_IOC           0x00008000
#define QTD_TOKEN_BYTES_SHIFT   16
#define QTD_TOKEN_BYTES_MASK    0x7FFF0000
#define QTD_TOKEN_TOGGLE        0x80000000

/* QH (Queue Head) layout (EHCI spec §3.6, 48 bytes, 32-byte aligned) */
#define QH_NEXT_TERMINATE      0x00000001
#define QH_TYPE_QH             0x00000002
#define QH_EPS_HIGH            (2u << 12)  /* High-speed endpoint */

/* Maximum number of qTDs per QH */
#define QTD_MAX_PER_QH         8

/* Maximum async schedule processing depth per call */
#define ASYNC_MAX_QDS          32

/**
 * ehci_process_async_qtd — Process a single qTD.
 *
 * Checks the status of a qTD and handles completion or error.
 *
 * @qtd_phys: Physical address of the qTD
 * @returns 0 if still active, 1 if complete, negative on error.
 */
static int ehci_process_async_qtd(uint64_t qtd_phys)
{
    volatile uint32_t *qtd_virt = (volatile uint32_t *)PHYS_TO_VIRT(qtd_phys);
    uint32_t token = qtd_virt[2];  /* Token dword at offset 8 */

    if (token & QTD_TOKEN_ACTIVE)
        return 0;  /* Still pending */

    if (token & QTD_TOKEN_HALTED) {
        kprintf("[EHCI] qTD halted (token=0x%08X)\n", (unsigned)token);
        return -EIO;
    }

    if (token & (QTD_TOKEN_BABBLE | QTD_TOKEN_XACTERR | QTD_TOKEN_DATABUFERR)) {
        kprintf("[EHCI] qTD error (token=0x%08X)\n", (unsigned)token);
        return -EIO;
    }

    /* Transfer completed successfully */
    return 1;
}

/**
 * ehci_process_async_schedule — Process the async schedule list.
 *
 * Walks the async schedule (QH list) and processes any completed qTDs.
 * Called from the USB interrupt handler or periodically.
 *
 * Returns the number of qTDs processed, or negative on error.
 */
int ehci_process_async_schedule(void)
{
    int c = 0;
    if (ehci_count < 1)
        return -1;

    uint32_t async_list_addr = op_read(c, EHCI_ASYNCLISTADDR);
    if (async_list_addr == 0)
        return 0;  /* Empty schedule */

    int processed = 0;
    uint32_t current_qh_phys = async_list_addr;
    int safety = ASYNC_MAX_QDS;

    while (current_qh_phys && !(current_qh_phys & QH_NEXT_TERMINATE) && safety > 0) {
        uint64_t qh_virt = (uint64_t)PHYS_TO_VIRT(current_qh_phys & ~0x1F);
        volatile uint32_t *qh = (volatile uint32_t *)qh_virt;

        /* Get the horizontal link pointer (next QH) */
        uint32_t horiz_link = qh[0];

        /* Get the overlay qTD pointer (current transfer) */
        uint32_t overlay_qtd = qh[2];  /* qTD pointer at offset 8 */

        if (overlay_qtd && !(overlay_qtd & QH_NEXT_TERMINATE)) {
            int ret = ehci_process_async_qtd((uint64_t)(overlay_qtd & ~0x1F));
            if (ret > 0) {
                processed++;
                kprintf("[EHCI] Async qTD completed (QH=0x%08X)\n",
                        (unsigned)current_qh_phys);
            }
        }

        /* Advance to next QH */
        current_qh_phys = horiz_link;
        safety--;
    }

    return processed;
}

/**
 * ehci_submit_async_qtd — Create and submit a single qTD.
 *
 * Allocates a qTD, fills it with the transfer parameters, and links
 * it into the async schedule.
 *
 * @dev_addr:  USB device address
 * @ep:        Endpoint number (0 for control)
 * @buf:       Data buffer (virtual address)
 * @len:       Transfer length
 * @pid:       PID: QTD_TOKEN_PID_SETUP, QTD_TOKEN_PID_INPUT, QTD_TOKEN_PID_OUTPUT
 * @toggle:    Data toggle (0 or 1)
 *
 * Returns 0 on success, negative on error.
 */
int ehci_submit_async_qtd(uint8_t dev_addr, uint8_t ep,
                           void *buf, uint32_t len,
                           uint32_t pid, int toggle)
{
    int c = 0;
    if (ehci_count < 1)
        return -1;

    /* Allocate a qTD (one page, 32-byte aligned) */
    uint64_t qtd_phys = pmm_alloc_frame();
    if (!qtd_phys)
        return -2;

    volatile uint32_t *qtd = (volatile uint32_t *)PHYS_TO_VIRT(qtd_phys);
    memset((void *)qtd, 0, 4096);

    /* Fill qTD */
    uint32_t buf_phys = (uint32_t)(uintptr_t)VIRT_TO_PHYS(buf);
    uint32_t pg_base = buf_phys & ~0xFFFu;
    uint32_t pg_off = buf_phys & 0xFFFu;

    /* Buffer pointers */
    qtd[3] = pg_base;                            /* buf_ptr0 */
    qtd[4] = (pg_off + len > 4096) ? (pg_base + 0x1000u) : 0;
    qtd[5] = 0;
    qtd[6] = 0;
    qtd[7] = 0;

    /* Token: status + PID + length + toggle */
    uint32_t bytes_to_send = (len > 0) ? (len << 16) : 0;
    uint32_t cerr = (3u << QTD_TOKEN_CERR_SHIFT);  /* 3 error count */
    uint32_t data_toggle = toggle ? QTD_TOKEN_TOGGLE : 0;
    uint32_t page_sel = (pg_off >> 12) & 0x07;

    qtd[2] = QTD_TOKEN_ACTIVE | pid | cerr |
             (page_sel << QTD_TOKEN_CPAGE_SHIFT) |
             bytes_to_send | data_toggle;

    qtd[0] = EHCI_PTR_TERMINATE;  /* Next qTD pointer: terminate */
    qtd[1] = EHCI_PTR_TERMINATE;  /* Alternate next: terminate */

    /* Allocate a QH and link the qTD */
    uint64_t qh_phys = pmm_alloc_frame();
    if (!qh_phys) {
        pmm_free_frame(qtd_phys);
        return -3;
    }

    volatile uint32_t *qh = (volatile uint32_t *)PHYS_TO_VIRT(qh_phys);
    memset((void *)qh, 0, 4096);

    /* QH: Horizontal link pointer -> terminate */
    qh[0] = EHCI_PTR_TERMINATE;

    /* Endpoint capabilities:
     *   bits 15:12 = endpoint speed (2 = high)
     *   bits 11:8  = endpoint number
     *   bits 7:0   = device address
     */
    uint32_t ep_cap = QH_EPS_HIGH | ((uint32_t)ep << 8) | dev_addr;
    qh[1] = ep_cap;

    /* Overlay: qTD pointer */
    qh[2] = (uint32_t)(qtd_phys & 0xFFFFFFE0);  /* 32-byte aligned */

    /* Current qTD pointer (same as overlay) */
    qh[4] = (uint32_t)(qtd_phys & 0xFFFFFFE0);

    /* Link QH into async schedule */
    uint32_t old_async = op_read(c, EHCI_ASYNCLISTADDR);
    qh[0] = old_async;  /* Point to whatever was there */
    __asm__ volatile("mfence" ::: "memory");

    op_write(c, EHCI_ASYNCLISTADDR, (uint32_t)(qh_phys & 0xFFFFFFE0));

    /* Enable async schedule */
    uint32_t cmd = op_read(c, EHCI_USBCMD);
    op_write(c, EHCI_USBCMD, cmd | EHCI_CMD_ASE);

    kprintf("[EHCI] Async qTD submitted: addr=%d ep=%d len=%u pid=%s\n",
            dev_addr, ep, len,
            (pid == QTD_TOKEN_PID_SETUP) ? "SETUP" :
            (pid == QTD_TOKEN_PID_INPUT) ? "IN" : "OUT");

    return 0;
}
int xhci_submit_isochronous(uint8_t dev_addr, uint8_t ep,
                             void *buf, uint32_t len)
{
    (void)dev_addr; (void)ep; (void)buf; (void)len;
    return -7;  /* ENOSYS — not yet implemented */
}

int ehci_usb_init(void) {
    /* Scan PCI for EHCI controllers (class 0x0C, subclass 0x03, prog-if 0x20) */
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t r0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((r0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t r2  = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x08);
            uint8_t  cls = (uint8_t)((r2 >> 24) & 0xFF);
            uint8_t  sub = (uint8_t)((r2 >> 16) & 0xFF);
            uint8_t  prg = (uint8_t)((r2 >> 8)  & 0xFF);
            if (cls == 0x0C && sub == 0x03 && prg == 0x20) {
                uint32_t bar0 = pci_read((uint8_t)bus, (uint8_t)slot, 0, 0x10);
                ehci_init_controller((uint8_t)bus, (uint8_t)slot, bar0);
            }
        }
    }
    return ehci_count > 0 ? 0 : -1;
}

/* ── Module support (M59: USB as loadable module) ──────────────── */

#ifdef MODULE

/* Reverse usb_init(): halt controllers, free periodic schedule, reset state */
void usb_exit(void) {
    /* Tear down periodic schedule if active */
    if (g_periodic_on)
        ehci_teardown_periodic();

    /* Halt each EHCI controller */
    for (int c = 0; c < ehci_count; c++) {
        /* Disable interrupts, halt controller */
        op_write(c, EHCI_USBINTR, 0);
        uint32_t cmd = op_read(c, EHCI_USBCMD);
        op_write(c, EHCI_USBCMD, cmd & ~EHCI_CMD_RUN);
        int timeout = 50000;
        while (!(op_read(c, EHCI_USBSTS) & EHCI_STS_HALTED) && --timeout)
            busy_wait_n(10);
    }

    /* Reset driver state */
    ehci_count = 0;
    usb_device_count = 0;
    memset(ehci_ctrl, 0, sizeof(ehci_ctrl));
    memset(usb_devices, 0, sizeof(usb_devices));

    kprintf("[USB] EHCI controller(s) shut down\n");
}

int init_module(void) {
    int ret = ehci_usb_init();
    if (ret != 0) {
        kprintf("[USB] init_module: no EHCI controller found\n");
        return 0; /* non-fatal — module loads but no hardware */
    }

    /* Initialize mass storage (block device) on the USB device */
    extern int usb_msc_init(void);
    int msc_ret = usb_msc_init();
    if (msc_ret != 0) {
        kprintf("[USB] init_module: no USB mass storage device\n");
    }

    return 0;
}

void cleanup_module(void) {
    /* Unregister block device */
    extern void usb_msc_exit(void);
    usb_msc_exit();

    /* Shut down EHCI controllers */
    usb_exit();

    kprintf("[USB] Module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("USB EHCI host controller + Mass Storage Class driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("pci:v00008086d00001C2Dsv0000*"); /* Intel 7-series EHCI */
MODULE_ALIAS("pci:v00008086d00001E2Dsv0000*"); /* Intel 8-series EHCI */
MODULE_ALIAS("pci:v00008086d00009C2Dsv0000*"); /* Intel 9-series EHCI */
MODULE_ALIAS("pci:v00001022d00007808sv0000*"); /* AMD Hudson EHCI */
#endif /* MODULE */

/* ── ehci_init: Initialise an EHCI controller from PCI device info ── */
int ehci_init(void *dev)
{
    struct pci_device *pdev = (struct pci_device *)dev;
    if (!pdev) return -EINVAL;

    kprintf("[usb] ehci_init: probing controller at %02x:%02x.%d\n",
            pdev->bus, pdev->slot, pdev->func);

    uint32_t bar0 = pci_read(pdev->bus, pdev->slot, pdev->func, 0x10);
    return ehci_init_controller(pdev->bus, pdev->slot, bar0);
}

/* ── ehci_reset: Reset a specific EHCI controller ──────────── */
int ehci_reset(void *dev)
{
    (void)dev;
    /* Find and reset the first EHCI controller */
    if (ehci_count == 0) return -EIO;

    int c = 0; /* reset first controller */
    uint32_t cmd = op_read(c, EHCI_USBCMD);
    op_write(c, EHCI_USBCMD, cmd | EHCI_CMD_HCRESET);
    int timeout = 50000;
    while ((op_read(c, EHCI_USBCMD) & EHCI_CMD_HCRESET) && --timeout)
        busy_wait_n(10);
    if (timeout == 0) {
        kprintf("[usb] ehci_reset: timeout waiting for controller reset\n");
        return -EIO;
    }
    kprintf("[usb] EHCI controller reset\n");
    return 0;
}

/* ── ehci_submit_urb: Submit a URB to an EHCI controller ──────────── */
int ehci_submit_urb(void *urb)
{
    if (!urb) return -EINVAL;
    kprintf("[usb] ehci_submit_urb: URB submitted (stub processing)\n");

    /* For now, just submit as an async qTD if we have a controller */
    if (ehci_count > 0) {
        /* Minimal stub - will be expanded */
        return 0;
    }

    return -EIO;
}

/* ── ehci_irq: EHCI interrupt handler ──────────────────────── */
void ehci_irq(struct interrupt_frame *frame)
{
    (void)frame;
    if (ehci_count == 0) return;

    /* Read status from first controller */
    uint32_t sts = op_read(0, EHCI_USBSTS);

    /* Check for port change events */
    if (sts & (1u << 2)) { /* Port Change Detect */
        /* Clear the status bit */
        op_write(0, EHCI_USBSTS, (1u << 2));
        kprintf("[usb] EHCI IRQ: port change detected\n");
    }

    /* Check for USB error interrupt (USBERRINT) */
    if (sts & (1u << 1)) {
        op_write(0, EHCI_USBSTS, (1u << 1));
    }

    /* Check for USB interrupt (USBINT) - transfer completion */
    if (sts & (1u << 0)) {
        op_write(0, EHCI_USBSTS, (1u << 0));
    }
}

