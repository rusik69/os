/*
 * usb_debug.c — xHCI DbC (Debug Capability) early kernel console
 *
 * Implements the xHCI Debug Capability (DbC) as defined in the xHCI
 * specification (§7.6).  DbC provides a USB 3.0 Debug Device that
 * appears as a vendor-specific USB device when connected to a host PC
 * running a debugger tool (e.g., Microsoft WinUSB or Linux usb_debug).
 *
 * This driver hooks into the kernel console system to provide an early
 * output channel via the DbC bulk OUT endpoint.  No input (bulk IN)
 * is currently processed, but the infrastructure is in place.
 *
 * Item S50 — USB debug port / xHCI DbC driver
 */

#include "usb.h"
#include "printf.h"
#include "string.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "timer.h"
#include "errno.h"

/* ── xHCI MMIO registers (for DbC detection) ────────────────────── */
#define XHCI_CAPLENGTH     0x00    /* Capability length (1 byte) */
#define XHCI_HCIVERSION    0x02    /* Interface version */
#define XHCI_HCCPARAMS1    0x10    /* Capability parameters 1 */
#define XHCI_DBOFF          0x14    /* DbC offset register */

/* HCCPARAMS1 bits */
#define XHCI_HCC1_DBC       (1u << 1)   /* Debug Capability present */

/* DbC Register Space (relative to DBCOFF) */
#define DBC_DCCTRL         0x00    /* Debug Control */
#define DBC_DCST           0x04    /* Debug Status */
#define DBC_DCPORTSC       0x08    /* Debug Port Status/Control */
#define DBC_DCCP           0x0C    /* Debug Context Pointer */

/* DCCTRL bits */
#define DBC_CTRL_LSE        (1u << 31)  /* Link State Enable */
#define DBC_CTRL_DCE        (1u << 0)   /* Debug Capability Enable */
#define DBC_CTRL_DCR        (1u << 1)   /* Debug Capability Reset */

/* DCST bits */
#define DBC_STS_CONNECTION   (1u << 0)   /* Device connected */
#define DBC_STS_HCHANGED     (1u << 1)   /* Host changed */

/* DCPORTSC bits */
#define DBC_PORTSC_CCS       (1u << 0)   /* Current Connect Status */
#define DBC_PORTSC_PED       (1u << 1)   /* Port Enabled */
#define DBC_PORTSC_PR        (1u << 4)   /* Port Reset */

/* DbC endpoint context sizes */
#define DBC_MAX_PACKET         1024
#define DBC_TRB_RING_SIZE      16      /* Number of TRBs in ring */
#define DBC_CTX_SIZE           (3 * 16 + 2 * 4)  /* ~56 bytes per context */

/* ── DbC device state ────────────────────────────────────────────── */
struct dbc_device {
    uint64_t mmio_base;          /* xHCI MMIO base */
    uint64_t dbc_offset;         /* Offset to DbC registers */

    /* DbC registers (MMIO pointers) */
    volatile uint32_t *dcctrl;
    volatile uint32_t *dcst;
    volatile uint32_t *dcportsc;
    volatile uint64_t *dccp;

    /* DbC context areas (allocated DMA memory) */
    void   *dbc_ctx;             /* DbC context (8-byte aligned) */
    uint64_t dbc_ctx_phys;

    /* Bulk OUT endpoint */
    struct dbc_trb_ring {
        uint64_t *trbs;          /* TRB ring (phys addr) */
        uint64_t  trbs_phys;
        int       enqueue_idx;
        int       dequeue_idx;
    } bulk_out;

    /* Bulk IN endpoint */
    struct dbc_trb_ring bulk_in;

    uint8_t  dbc_inited;         /* 1 if DbC initialized */
    uint8_t  dbc_connected;      /* 1 if host debugger connected */
    spinlock_t lock;
};

/* ── TRB structure (matching xHCI spec) ──────────────────────────── */
struct dbc_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

/* TRB types for DbC */
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_LINK           6

/* TRB control flags */
#define TRB_C                   0x00000001  /* Cycle bit */
#define TRB_CHAIN               0x00000020
#define TRB_ENT                 0x00000040
#define TRB_ISP                 0x00000080  /* Interrupt on Short Packet */
#define TRB_IOC                 0x00000100  /* Interrupt on Complete */
#define TRB_TYPE_SHIFT          10

/* ── Globals ─────────────────────────────────────────────────────── */
static struct dbc_device g_dbc;
static int g_dbc_initialized = 0;

/* ── Scratch buffer for console output ───────────────────────────── */
#define DBC_CONSOLE_BUF_SIZE    2048
static char g_dbc_console_buf[DBC_CONSOLE_BUF_SIZE];
static int  g_dbc_console_pos = 0;

/* ── MMIO helpers ────────────────────────────────────────────────── */
static inline uint32_t dbc_read32(struct dbc_device *dbc, uint32_t reg)
{
    return *(volatile uint32_t *)(dbc->mmio_base + reg);
}

static inline void dbc_write32(struct dbc_device *dbc, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(dbc->mmio_base + reg) = val;
}

/* ── TRB ring management ─────────────────────────────────────────── */

static int dbc_init_trb_ring(struct dbc_trb_ring *ring, int n_trbs)
{
    /* Allocate physically contiguous memory for TRB ring (aligned to 16 bytes) */
    size_t ring_size = (size_t)n_trbs * sizeof(struct dbc_trb);
    /* Allocate frame(s) for ring */
    uint32_t num_frames = (uint32_t)((ring_size + 0xFFF) / 0x1000);
    if (num_frames == 0) num_frames = 1;

    ring->trbs_phys = pmm_alloc_frame();
    if (!ring->trbs_phys)
        return -1;
    ring->trbs = (uint64_t *)PHYS_TO_VIRT(ring->trbs_phys);
    memset(ring->trbs, 0, (size_t)num_frames * 0x1000);

    /* Set up a Link TRB at the end pointing back to start */
    struct dbc_trb *last = (struct dbc_trb *)&ring->trbs[(n_trbs - 1) * 2];
    last->parameter = ring->trbs_phys;
    last->control = TRB_C | (TRB_TYPE_LINK << TRB_TYPE_SHIFT);

    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    return 0;
}

static void dbc_flush_output(struct dbc_device *dbc)
{
    if (g_dbc_console_pos <= 0 || !dbc->dbc_connected)
        return;

    spinlock_acquire(&dbc->lock);

    /* Submit a Normal TRB for the console buffer */
    int idx = dbc->bulk_out.enqueue_idx;
    struct dbc_trb *trbs = (struct dbc_trb *)dbc->bulk_out.trbs;
    struct dbc_trb *trb = &trbs[idx];

    /* Use the console buffer directly (for simplicity) */
    uint64_t buf_phys = (uint64_t)g_dbc_console_buf;  /* assumes identity map */
    trb->parameter = buf_phys;
    trb->status = (uint32_t)g_dbc_console_pos;  /* TRB transfer length */
    trb->control = TRB_C | (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;

    /* Ring the doorbell (write to DCCP with endpoint context index) */
    /* Endpoint context index 1 = DbC OUT endpoint */
    dbc_write32(dbc, (uint32_t)dbc->dbc_offset + 0x10, 1);

    dbc->bulk_out.enqueue_idx = (idx + 1) % DBC_TRB_RING_SIZE;
    g_dbc_console_pos = 0;

    spinlock_release(&dbc->lock);
}

/* ── Console output hook ─────────────────────────────────────────── */

static void dbc_console_write(const char *buf, int len)
{
    if (!g_dbc_initialized || !g_dbc.dbc_connected)
        return;

    while (len > 0) {
        int space = DBC_CONSOLE_BUF_SIZE - g_dbc_console_pos;
        if (space <= 0) {
            dbc_flush_output(&g_dbc);
            space = DBC_CONSOLE_BUF_SIZE;
        }
        int copy = len < space ? len : space;
        memcpy(g_dbc_console_buf + g_dbc_console_pos, buf, (size_t)copy);
        g_dbc_console_pos += copy;
        buf += copy;
        len -= copy;
    }
}

/* ── DbC initialization sequence ─────────────────────────────────── */

/*
 * Probe for xHCI controller with Debug Capability (DbC).
 * Returns 0 on success, -1 if no DbC found.
 */
static int dbc_probe_xhci(struct dbc_device *dbc)
{
    /* Scan PCI for an xHCI controller */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0);
                if (id == 0xFFFFFFFF || id == 0)
                    continue;

                uint8_t class_code = (uint8_t)(pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x0B) & 0xFF);
                uint8_t subclass   = (uint8_t)(pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x0A) & 0xFF);
                uint8_t prog_if    = (uint8_t)(pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x09) & 0xFF);

                /* xHCI: class=0x0C, subclass=0x03, prog_if=0x30 */
                if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x30) {
                    uint32_t bar0 = pci_read((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x10);
                    uint64_t mmio_base = (uint64_t)(bar0 & 0xFFFFFFF0);
                    dbc->mmio_base = mmio_base;

                    /* Check for DbC capability */
                    uint8_t caplength = dbc_read32(dbc, XHCI_CAPLENGTH) & 0xFF;
                    uint32_t hccparams1 = dbc_read32(dbc, (uint32_t)(caplength + XHCI_HCCPARAMS1));
                    if (hccparams1 & XHCI_HCC1_DBC) {
                        /* Read DbC offset */
                        uint32_t dboff = dbc_read32(dbc, (uint32_t)(caplength + XHCI_DBOFF));
                        dbc->dbc_offset = dboff;
                        dbc->dcctrl   = (volatile uint32_t *)(mmio_base + dboff + DBC_DCCTRL);
                        dbc->dcst     = (volatile uint32_t *)(mmio_base + dboff + DBC_DCST);
                        dbc->dcportsc = (volatile uint32_t *)(mmio_base + dboff + DBC_DCPORTSC);
                        dbc->dccp     = (volatile uint64_t *)(mmio_base + dboff + DBC_DCCP);

                        kprintf("[DBC] xHCI controller with DbC at "
                                "PCI %02x:%02x.%d MMIO=0x%llx DBC_OFF=0x%x\n",
                                bus, dev, func,
                                (unsigned long long)mmio_base, dboff);
                        return 0;
                    }
                }
            }
        }
    }

    return -1;
}

/*
 * dbc_init — Initialize Debug Capability
 *
 * Sequence:
 *   1. Probe xHCI for DbC
 *   2. Allocate DbC context and endpoint context structures
 *   3. Set DCE (Debug Capability Enable)
 *   4. Wait for connection
 *   5. Initialize TRB rings for bulk OUT/IN
 */
static int dbc_init(void)
{
    if (g_dbc_initialized)
        return 0;

    struct dbc_device *dbc = &g_dbc;
    memset(dbc, 0, sizeof(*dbc));
    spinlock_init(&dbc->lock);

    /* 1. Probe xHCI */
    if (dbc_probe_xhci(dbc) < 0) {
        kprintf("[DBC] No xHCI DbC found\n");
        return -ENODEV;
    }

    /* 2. Allocate DbC context (8-byte aligned, ~128 bytes) */
    dbc->dbc_ctx_phys = pmm_alloc_frame();
    if (!dbc->dbc_ctx_phys) {
        kprintf("[DBC] Failed to allocate DbC context\n");
        return -ENOMEM;
    }
    dbc->dbc_ctx = (void *)PHYS_TO_VIRT(dbc->dbc_ctx_phys);
    memset(dbc->dbc_ctx, 0, 0x1000);

    /* 3. Write context pointer to DCCP */
    *dbc->dccp = dbc->dbc_ctx_phys;

    /* 4. Initialize bulk OUT TRB ring */
    if (dbc_init_trb_ring(&dbc->bulk_out, DBC_TRB_RING_SIZE) < 0) {
        kprintf("[DBC] Failed to allocate bulk OUT ring\n");
        return -ENOMEM;
    }

    /* 5. Initialize bulk IN TRB ring */
    if (dbc_init_trb_ring(&dbc->bulk_in, DBC_TRB_RING_SIZE) < 0) {
        kprintf("[DBC] Failed to allocate bulk IN ring\n");
        return -ENOMEM;
    }

    /* 6. Enable Debug Capability */
    *dbc->dcctrl = DBC_CTRL_DCE;

    /* 7. Wait briefly for connection to establish */
    uint64_t timeout = timer_get_ticks() + TIMER_FREQ / 10;  /* 100 ms */
    while (timer_get_ticks() < timeout) {
        uint32_t dcst = *dbc->dcst;
        if (dcst & DBC_STS_CONNECTION) {
            dbc->dbc_connected = 1;
            break;
        }
        __asm__ volatile("pause");
    }

    if (dbc->dbc_connected) {
        kprintf("[DBC] Debug host connected\n");
    } else {
        kprintf("[DBC] Waiting for debug host connection...\n");
    }

    g_dbc_initialized = 1;
    kprintf("[OK] USB Debug port (DbC) initialized\n");
    return 0;
}

/* ── Connection status ───────────────────────────────────────────── */

static int dbc_is_connected(void)
{
    if (!g_dbc_initialized)
        return 0;
    return g_dbc.dbc_connected ? 1 : 0;
}

static int dbc_is_present(void)
{
    return g_dbc_initialized;
}

/* ── Stub: usb_debug_init ─────────────────────────────── */
static int usb_debug_init(void *dev)
{
    (void)dev;
    kprintf("[USB] usb_debug_init: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_debug_write ─────────────────────────────── */
static int usb_debug_write(void *dev, const void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[USB] usb_debug_write: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_debug_read ─────────────────────────────── */
static int usb_debug_read(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[USB] usb_debug_read: not yet implemented\n");
    return 0;
}
