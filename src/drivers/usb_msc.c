/*
 * USB Mass Storage Class — Bulk-Only Transport (BOT) driver
 *
 * Uses EHCI async queue to submit control and bulk transfers to a
 * USB flash drive.  After successful initialisation the device is
 * registered as BLOCKDEV_USB0 so that fat32_mount(FAT32_DISK_USB0)
 * works.
 *
 * References:
 *   USB Mass Storage Class — Bulk-Only Transport, Revision 1.0
 *   SCSI Primary Commands (SPC-4), READ(10), WRITE(10), READ CAPACITY(10)
 *   EHCI Specification, Revision 1.0
 */

#include "usb_msc.h"
#include "usb.h"
#include "blockdev.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* ── EHCI TD / QH structures ─────────────────────────────────────────────── */

/* Queue Element Transfer Descriptor (qTD) — EHCI spec §3.5 */
struct ehci_qtd {
    uint32_t next;           /* next qTD pointer (or 1 = terminate) */
    uint32_t alt_next;       /* alternate next (or 1) */
    uint32_t token;          /* status, PID, length, etc. */
    uint32_t buf[5];         /* up to 5×4 kB buffer pages */
    /* padding to 32-byte alignment */
    uint32_t _pad[3];
} __attribute__((packed, aligned(32)));

/* Queue Head (qH) — EHCI spec §3.6 */
struct ehci_qh {
    uint32_t next_qh;        /* horizontal link (T=1 for end) */
    uint32_t ep_char;        /* endpoint characteristics */
    uint32_t ep_cap;         /* endpoint capabilities */
    uint32_t cur_qtd;        /* current qTD pointer */
    /* overlay — mirrors qTD */
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t buf[5];
    uint32_t _pad[3];
} __attribute__((packed, aligned(32)));

/* qTD token field bits */
#define QTD_STATUS_ACTIVE   (1u << 7)
#define QTD_STATUS_HALTED   (1u << 6)
#define QTD_STATUS_MASK     0xFFu
#define QTD_PID_OUT         (0u << 8)
#define QTD_PID_IN          (1u << 8)
#define QTD_PID_SETUP       (2u << 8)
#define QTD_IOC             (1u << 15)
#define QTD_C_PAGE(x)       ((x) << 12)
#define QTD_CERR(x)         ((x) << 10)
#define QTD_BYTES(x)        ((uint32_t)(x) << 16)
#define QTD_DT              (1u << 31)

/* qH ep_char bits */
#define QH_DEVADDR(a)       ((a) & 0x7F)
#define QH_EP(n)            (((n) & 0xF) << 8)
#define QH_EPS_HS           (2u << 12)   /* high-speed */
#define QH_DTC              (1u << 14)   /* data toggle control */
#define QH_H                (1u << 15)   /* head of reclamation list */
#define QH_MAXPKT(n)        (((n) & 0x7FF) << 16)
#define QH_RL(n)            (((n) & 0xF) << 28)
#define QH_MULT(n)          (((n) & 3u) << 30)

/* EHCI Operational Register offsets (same as ehci driver) */
#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CMD_ASE        (1u << 5)
#define EHCI_CMD_RUN        (1u << 0)
#define EHCI_STS_ASS        (1u << 15)

/* ── BOT Command Block Wrapper / Status Wrapper ───────────────────────────── */

#define BOT_CBW_SIGNATURE   0x43425355u
#define BOT_CSW_SIGNATURE   0x53425355u

struct bot_cbw {
    uint32_t signature;      /* 0x43425355 */
    uint32_t tag;
    uint32_t data_len;       /* bytes host expects to transfer */
    uint8_t  flags;          /* 0x80 = data in, 0x00 = data out */
    uint8_t  lun;
    uint8_t  cb_len;         /* SCSI command length (6–16) */
    uint8_t  cb[16];         /* SCSI command block */
} __attribute__((packed));

struct bot_csw {
    uint32_t signature;      /* 0x53425355 */
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;         /* 0 = good, 1 = failed, 2 = phase error */
} __attribute__((packed));

/* ── Driver state ────────────────────────────────────────────────────────── */

static uint64_t g_op_base     = 0;
static uint8_t  g_dev_addr    = 1;   /* USB address assigned after reset */
static uint8_t  g_bulk_in_ep  = 1;   /* bulk IN endpoint number */
static uint8_t  g_bulk_out_ep = 2;   /* bulk OUT endpoint number */
static uint32_t g_max_lba     = 0;   /* total sectors - 1 */
static uint32_t g_tag         = 1;

#define MAX_PKT 512   /* high-speed bulk max packet size */

/* ── EHCI MMIO helpers ───────────────────────────────────────────────────── */
static inline uint32_t op_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_op_base + off);
}
static inline void op_wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_op_base + off) = val;
}

static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── Allocate a physically-contiguous buffer (page-aligned via pmm) ─────── */
static void *alloc_dma(size_t sz) {
    /* pmm_alloc_frame gives a 4 kB page — sufficient for all our buffers */
    (void)sz;
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return (void *)0;
    void *p = (void *)frame;
    memset(p, 0, 4096);
    return p;
}

/* ── Submit one async transfer via EHCI ──────────────────────────────────── */
/*
 * Sets up a single qH→qTD chain, programs the async list register, enables
 * the async schedule, polls until the qTD completes, then disables.
 *
 * pid      : QTD_PID_SETUP / QTD_PID_IN / QTD_PID_OUT
 * ep       : endpoint number
 * data     : buffer (must be < 4 kB for simplicity)
 * len      : transfer length in bytes
 * toggle   : initial data toggle (0 or 1)
 * Returns 0 on success, negative on error.
 */
static int ehci_do_transfer(uint32_t pid, uint8_t ep, void *data,
                            uint32_t len, int toggle)
{
    if (!g_op_base) return -1;

    struct ehci_qh  *qh  = (struct ehci_qh  *)alloc_dma(sizeof(*qh));
    struct ehci_qtd *qtd = (struct ehci_qtd *)alloc_dma(sizeof(*qtd));
    if (!qh || !qtd) return -2;

    /* Build qTD */
    uint32_t token = QTD_STATUS_ACTIVE
                   | QTD_CERR(3)
                   | pid
                   | QTD_BYTES(len)
                   | QTD_IOC;
    if (toggle) token |= QTD_DT;

    qtd->next     = 1;   /* terminate */
    qtd->alt_next = 1;
    qtd->token    = token;
    qtd->buf[0]   = (uint32_t)(uint64_t)data;
    qtd->buf[1]   = ((uint32_t)(uint64_t)data & ~0xFFFu) + 0x1000u;
    qtd->buf[2]   = qtd->buf[1] + 0x1000u;
    qtd->buf[3]   = qtd->buf[2] + 0x1000u;
    qtd->buf[4]   = qtd->buf[3] + 0x1000u;

    /* Build qH — points to itself (circular list of one) */
    uint32_t qh_phys = (uint32_t)(uint64_t)qh;
    qh->next_qh  = qh_phys | 0;   /* T=0, type=00 (qH) */
    qh->ep_char  = QH_DEVADDR(g_dev_addr)
                 | QH_EP(ep)
                 | QH_EPS_HS
                 | QH_DTC
                 | QH_H
                 | QH_MAXPKT(MAX_PKT)
                 | QH_RL(4);
    qh->ep_cap   = QH_MULT(1);
    qh->cur_qtd  = 0;
    qh->next_qtd = (uint32_t)(uint64_t)qtd;   /* first qTD */
    qh->alt_qtd  = 1;
    qh->token    = 0;

    /* Programme async list */
    uint32_t old_async = op_rd(EHCI_ASYNCLISTADDR);
    uint32_t old_cmd   = op_rd(EHCI_USBCMD);

    op_wr(EHCI_ASYNCLISTADDR, qh_phys);

    /* Enable async schedule */
    op_wr(EHCI_USBCMD, old_cmd | EHCI_CMD_ASE);
    int timeout = 200000;
    while (!(op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);
    if (!timeout) goto fail;

    /* Poll for qTD completion */
    timeout = 2000000;
    while ((qtd->token & QTD_STATUS_ACTIVE) && --timeout)
        busy_wait(10);

    /* Disable async schedule */
    op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    timeout = 200000;
    while ((op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);

    /* Restore old async list pointer */
    op_wr(EHCI_ASYNCLISTADDR, old_async);

    int rc = 0;
    if (qtd->token & QTD_STATUS_MASK & ~QTD_STATUS_ACTIVE) {
        if (qtd->token & QTD_STATUS_HALTED) rc = -3;
    }

    pmm_free_frame((uint64_t)qtd);
    pmm_free_frame((uint64_t)qh);
    return rc;

fail:
    op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    op_wr(EHCI_ASYNCLISTADDR, old_async);
    pmm_free_frame((uint64_t)qtd);
    pmm_free_frame((uint64_t)qh);
    return -4;
}

/* ── Send a USB control request (SETUP + optional DATA + STATUS) ─────────── */
/*
 * Standard SETUP packet layout:
 *   bmRequestType, bRequest, wValue(lo,hi), wIndex(lo,hi), wLength(lo,hi)
 */
static int usb_control(uint8_t bm_req_type, uint8_t b_req,
                       uint16_t w_val, uint16_t w_idx, uint16_t w_len,
                       void *data)
{
    /* 8-byte SETUP packet */
    uint8_t *setup = (uint8_t *)alloc_dma(8);
    if (!setup) return -1;
    setup[0] = bm_req_type;
    setup[1] = b_req;
    setup[2] = (uint8_t)(w_val & 0xFF);
    setup[3] = (uint8_t)(w_val >> 8);
    setup[4] = (uint8_t)(w_idx & 0xFF);
    setup[5] = (uint8_t)(w_idx >> 8);
    setup[6] = (uint8_t)(w_len & 0xFF);
    setup[7] = (uint8_t)(w_len >> 8);

    /* SETUP phase (toggle=0, PID_SETUP) */
    int rc = ehci_do_transfer(QTD_PID_SETUP, 0, setup, 8, 0);
    pmm_free_frame((uint64_t)setup);
    if (rc < 0) return rc;

    /* DATA phase (toggle=1) */
    if (w_len && data) {
        uint8_t pid = (bm_req_type & 0x80) ? QTD_PID_IN : QTD_PID_OUT;
        rc = ehci_do_transfer(pid, 0, data, w_len, 1);
        if (rc < 0) return rc;
    }

    /* STATUS phase — opposite direction, DATA1 */
    {
        uint8_t pid = (bm_req_type & 0x80) ? QTD_PID_OUT : QTD_PID_IN;
        uint8_t *dummy = (uint8_t *)alloc_dma(4);
        if (!dummy) return -1;
        rc = ehci_do_transfer(pid, 0, dummy, 0, 1);
        pmm_free_frame((uint64_t)dummy);
    }
    return rc;
}

/* ── BOT bulk transfer helpers ───────────────────────────────────────────── */

static int bot_send_cbw(struct bot_cbw *cbw)
{
    return ehci_do_transfer(QTD_PID_OUT, g_bulk_out_ep,
                            cbw, sizeof(*cbw), 0);
}

static int bot_recv_data(void *buf, uint32_t len)
{
    return ehci_do_transfer(QTD_PID_IN, g_bulk_in_ep, buf, len, 1);
}

static int bot_send_data(const void *buf, uint32_t len)
{
    return ehci_do_transfer(QTD_PID_OUT, g_bulk_out_ep,
                            (void *)buf, len, 0);
}

static int bot_recv_csw(struct bot_csw *csw)
{
    int rc = ehci_do_transfer(QTD_PID_IN, g_bulk_in_ep,
                              csw, sizeof(*csw), 1);
    if (rc < 0) return rc;
    if (csw->signature != BOT_CSW_SIGNATURE) return -10;
    if (csw->status != 0) return -11;
    return 0;
}

/* ── SCSI READ CAPACITY (10) ─────────────────────────────────────────────── */
static int scsi_read_capacity(uint32_t *max_lba_out)
{
    struct bot_cbw *cbw = (struct bot_cbw *)alloc_dma(sizeof(*cbw));
    struct bot_csw *csw = (struct bot_csw *)alloc_dma(sizeof(*csw));
    uint8_t        *buf = (uint8_t *)alloc_dma(8);
    if (!cbw || !csw || !buf) return -1;

    cbw->signature = BOT_CBW_SIGNATURE;
    cbw->tag       = g_tag++;
    cbw->data_len  = 8;
    cbw->flags     = 0x80;  /* data in */
    cbw->lun       = 0;
    cbw->cb_len    = 10;
    memset(cbw->cb, 0, 16);
    cbw->cb[0]     = 0x25; /* READ CAPACITY(10) */

    int rc = bot_send_cbw(cbw);
    if (rc == 0) rc = bot_recv_data(buf, 8);
    if (rc == 0) rc = bot_recv_csw(csw);

    if (rc == 0) {
        uint32_t lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                     | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
        *max_lba_out = lba;
    }

    pmm_free_frame((uint64_t)cbw);
    pmm_free_frame((uint64_t)csw);
    pmm_free_frame((uint64_t)buf);
    return rc;
}

/* ── SCSI READ (10) ──────────────────────────────────────────────────────── */
static int scsi_read10(uint32_t lba, uint8_t count, void *buf)
{
    struct bot_cbw *cbw = (struct bot_cbw *)alloc_dma(sizeof(*cbw));
    struct bot_csw *csw = (struct bot_csw *)alloc_dma(sizeof(*csw));
    if (!cbw || !csw) return -1;

    cbw->signature = BOT_CBW_SIGNATURE;
    cbw->tag       = g_tag++;
    cbw->data_len  = (uint32_t)count * 512;
    cbw->flags     = 0x80; /* data in */
    cbw->lun       = 0;
    cbw->cb_len    = 10;
    memset(cbw->cb, 0, 16);
    cbw->cb[0] = 0x28; /* READ(10) */
    cbw->cb[2] = (uint8_t)(lba >> 24);
    cbw->cb[3] = (uint8_t)(lba >> 16);
    cbw->cb[4] = (uint8_t)(lba >> 8);
    cbw->cb[5] = (uint8_t)(lba);
    cbw->cb[7] = 0;
    cbw->cb[8] = count;

    int rc = bot_send_cbw(cbw);
    if (rc == 0) rc = bot_recv_data(buf, (uint32_t)count * 512);
    if (rc == 0) rc = bot_recv_csw(csw);

    pmm_free_frame((uint64_t)cbw);
    pmm_free_frame((uint64_t)csw);
    return rc;
}

/* ── SCSI WRITE (10) ─────────────────────────────────────────────────────── */
static int scsi_write10(uint32_t lba, uint8_t count, const void *buf)
{
    struct bot_cbw *cbw = (struct bot_cbw *)alloc_dma(sizeof(*cbw));
    struct bot_csw *csw = (struct bot_csw *)alloc_dma(sizeof(*csw));
    if (!cbw || !csw) return -1;

    cbw->signature = BOT_CBW_SIGNATURE;
    cbw->tag       = g_tag++;
    cbw->data_len  = (uint32_t)count * 512;
    cbw->flags     = 0x00; /* data out */
    cbw->lun       = 0;
    cbw->cb_len    = 10;
    memset(cbw->cb, 0, 16);
    cbw->cb[0] = 0x2A; /* WRITE(10) */
    cbw->cb[2] = (uint8_t)(lba >> 24);
    cbw->cb[3] = (uint8_t)(lba >> 16);
    cbw->cb[4] = (uint8_t)(lba >> 8);
    cbw->cb[5] = (uint8_t)(lba);
    cbw->cb[7] = 0;
    cbw->cb[8] = count;

    int rc = bot_send_cbw(cbw);
    if (rc == 0) rc = bot_send_data(buf, (uint32_t)count * 512);
    if (rc == 0) rc = bot_recv_csw(csw);

    pmm_free_frame((uint64_t)cbw);
    pmm_free_frame((uint64_t)csw);
    return rc;
}

/* ── Blockdev callbacks ───────────────────────────────────────────────────── */

int usb_msc_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    return scsi_read10(lba, count, buf);
}

int usb_msc_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    return scsi_write10(lba, count, buf);
}

uint32_t usb_msc_get_sectors(void)
{
    return g_max_lba + 1;
}

/* ── Public init ─────────────────────────────────────────────────────────── */

int usb_msc_init(void)
{
    if (!usb_is_present()) return -1;
    if (usb_get_device_count() == 0) return -2;

    g_op_base = ehci_get_op_base();
    if (!g_op_base) return -3;

    /*
     * The EHCI driver already reset the port and detected the device.
     * We assume the device is a high-speed MSC with standard endpoint
     * numbers (bulk-in=1, bulk-out=2).  For real hardware we would
     * read the configuration descriptor; that requires control transfers
     * to endpoint 0 which we do here via usb_control().
     */

    /* SET_ADDRESS (standard SETUP to ep0, device address = 1) */
    int rc = usb_control(0x00, 0x05, g_dev_addr, 0, 0, (void *)0);
    if (rc < 0) {
        kprintf("  USB MSC: SET_ADDRESS failed (%d)\n", (uint64_t)rc);
        return rc;
    }
    busy_wait(50000);

    /* GET_DESCRIPTOR — device descriptor (just to confirm it's alive) */
    uint8_t *desc = (uint8_t *)alloc_dma(18);
    if (!desc) return -4;
    rc = usb_control(0x80, 0x06, 0x0100, 0, 18, desc);
    if (rc < 0) {
        kprintf("  USB MSC: GET_DESCRIPTOR failed (%d)\n", (uint64_t)rc);
        pmm_free_frame((uint64_t)desc);
        return rc;
    }
    /* desc[4]=bDeviceClass, [14]=bNumConfigurations */
    pmm_free_frame((uint64_t)desc);

    /* SET_CONFIGURATION 1 */
    rc = usb_control(0x00, 0x09, 1, 0, 0, (void *)0);
    if (rc < 0) {
        kprintf("  USB MSC: SET_CONFIGURATION failed (%d)\n", (uint64_t)rc);
        return rc;
    }

    /* BOT Class Reset */
    rc = usb_control(0x21, 0xFF, 0, 0, 0, (void *)0);
    if (rc < 0) {
        /* Non-fatal — some devices ignore it */
        kprintf("  USB MSC: BOT reset warning (%d)\n", (uint64_t)rc);
    }

    /* READ CAPACITY to discover drive size */
    rc = scsi_read_capacity(&g_max_lba);
    if (rc < 0) {
        kprintf("  USB MSC: READ CAPACITY failed (%d)\n", (uint64_t)rc);
        return rc;
    }

    kprintf("  USB MSC: %u sectors (%u MB)\n",
            (uint64_t)(g_max_lba + 1),
            (uint64_t)((g_max_lba + 1) / 2048));

    /* Register with block device layer */
    blockdev_register(BLOCKDEV_USB0, "usb0",
                      usb_msc_read_sectors,
                      usb_msc_write_sectors,
                      usb_msc_get_sectors);
    return 0;
}
