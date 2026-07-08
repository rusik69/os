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
#include "errno.h"

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
    void *p = PHYS_TO_VIRT(frame);
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
    qtd->buf[0]   = (uint32_t)VIRT_TO_PHYS(data);
    qtd->buf[1]   = (VIRT_TO_PHYS(data) & ~0xFFFu) + 0x1000u;
    qtd->buf[2]   = qtd->buf[1] + 0x1000u;
    qtd->buf[3]   = qtd->buf[2] + 0x1000u;
    qtd->buf[4]   = qtd->buf[3] + 0x1000u;

    /* Build qH — points to itself (circular list of one) */
    uint32_t qh_phys = (uint32_t)VIRT_TO_PHYS(qh);
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
    qh->next_qtd = (uint32_t)VIRT_TO_PHYS(qtd);   /* first qTD */
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

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
    return rc;

fail:
    op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    op_wr(EHCI_ASYNCLISTADDR, old_async);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
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
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)setup));
    if (rc < 0) return rc;

    /* DATA phase (toggle=1) */
    if (w_len && data) {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_IN : QTD_PID_OUT);
        rc = ehci_do_transfer(pid, 0, data, w_len, 1);
        if (rc < 0) return rc;
    }

    /* STATUS phase — opposite direction, DATA1 */
    {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_OUT : QTD_PID_IN);
        uint8_t *dummy = (uint8_t *)alloc_dma(4);
        if (!dummy) return -1;
        rc = ehci_do_transfer(pid, 0, dummy, 0, 1);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)dummy));
    }
    return rc;
}

/* ── CLEAR_FEATURE(ENDPOINT_HALT) ────────────────────────────────────────── */
/*
 * USB 2.0 spec §9.4.1: Clear a feature on a device, interface, or endpoint.
 * For BBB stall recovery, we call CLEAR_FEATURE(ENDPOINT_HALT) on the bulk
 * endpoint that stalled.  The wIndex is the endpoint address (ep_num | dir).
 * Returns 0 on success, negative on error.
 */
static int usb_clear_halt(uint8_t dev_addr, uint8_t ep_addr)
{
    (void)dev_addr;
    /* bmRequestType = 0x02 (Standard, Endpoint, Host-to-Device) */
    return usb_control(0x02, USB_REQ_CLEAR_FEATURE,
                       USB_FEATURE_ENDPOINT_HALT, ep_addr, 0, (void *)0);
}

/* ── BOT Mass Storage Reset (class-specific request) ─────────────────────── */
/*
 * USB Mass Storage Class spec §3.1: Bulk-Only Transport Reset.
 * Resets the mass storage device's BBB state machine.
 * Returns 0 on success, negative on error.
 */
static int bot_reset(void)
{
    /* bmRequestType = 0x21 (Class, Interface, Host-to-Device)
     * bRequest = 0xFF (Bulk-Only Mass Storage Reset)
     * wValue = 0, wIndex = interface 0 */
    return usb_control(0x21, 0xFF, 0, 0, 0, (void *)0);
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
                            (void *)(uintptr_t)buf, len, 0);
}

static int bot_recv_csw(struct bot_csw *csw, struct bot_cbw *cbw)
{
    int rc = ehci_do_transfer(QTD_PID_IN, g_bulk_in_ep,
                              csw, sizeof(*csw), 1);
    if (rc < 0) return rc;

    /* Validate CSW (USB MSC spec §6.3) */
    if (csw->signature != BOT_CSW_SIGNATURE)
        return -EIO;
    if (cbw && csw->tag != cbw->tag)
        return -EPROTO;

    /* status: 0=good, 1=failed, 2=phase error */
    if (csw->status == 1)
        return -EIO;
    if (csw->status == 2)
        return -EPROTO;

    return 0;
}

/* ── SCSI command execution with stall recovery ──────────────────────────── */
/*
 * Execute a SCSI command over BBB, with automatic stall recovery:
 * 1. Send CBW
 * 2. Send/receive data (if wlen > 0)
 * 3. Receive CSW
 * 4. If any step stalls → do CLEAR_FEATURE on the stalled endpoint,
 *    do BOT reset, and retry once.
 *
 * @cdb:       SCSI command descriptor block (6–16 bytes)
 * @cdb_len:   CDB length in bytes
 * @data:      Data buffer (NULL for no data phase)
 * @data_len:  Data transfer length
 * @dir:       0 = data out (host→device), 1 = data in (device→host)
 * @tag:       CBW tag (output — filled from g_tag)
 * Returns 0 on success, negative errno on failure.
 */
static int scsi_execute(const uint8_t *cdb, int cdb_len,
                        void *data, uint32_t data_len, int dir)
{
    struct bot_cbw *cbw = (struct bot_cbw *)alloc_dma(sizeof(*cbw));
    struct bot_csw *csw = (struct bot_csw *)alloc_dma(sizeof(*csw));
    if (!cbw || !csw) {
        if (cbw) pmm_free_frame(VIRT_TO_PHYS((uint64_t)cbw));
        if (csw) pmm_free_frame(VIRT_TO_PHYS((uint64_t)csw));
        return -ENOMEM;
    }

    int rc;
    int stalled = 0;
    int max_retries = 2;

    do {
        /* Build CBW */
        cbw->signature = BOT_CBW_SIGNATURE;
        cbw->tag       = g_tag++;
        cbw->data_len  = data_len;
        cbw->flags     = (uint8_t)(dir ? 0x80 : 0x00);
        cbw->lun       = 0;
        cbw->cb_len    = (uint8_t)(cdb_len & 0xFF);
        memset(cbw->cb, 0, 16);
        memcpy(cbw->cb, cdb, (size_t)(cdb_len < 16 ? cdb_len : 16));

        rc = bot_send_cbw(cbw);
        if (rc < 0) {
            /* Out endpoint stalled — clear halt + reset */
            rc = usb_clear_halt(g_dev_addr, g_bulk_out_ep);
            if (rc == 0) rc = bot_reset();
            if (rc == 0) stalled = 1;
            continue;
        }

        if (data_len && data) {
            if (dir) {
                rc = bot_recv_data(data, data_len);
            } else {
                rc = bot_send_data(data, data_len);
            }
            if (rc < 0) {
                /* Determine which endpoint stalled */
                uint8_t stalled_ep = dir ? g_bulk_in_ep : g_bulk_out_ep;
                rc = usb_clear_halt(g_dev_addr, stalled_ep);
                if (rc == 0) rc = bot_reset();
                if (rc == 0) stalled = 1;
                continue;
            }
        }

        rc = bot_recv_csw(csw, cbw);
        if (rc < 0) {
            /* CSW phase error — BOT reset required */
            usb_clear_halt(g_dev_addr, g_bulk_in_ep);
            bot_reset();
            stalled = 1;
            continue;
        }
        rc = 0;  /* success */
        break;

    } while (stalled && --max_retries > 0);

    if (rc != 0) {
        kprintf("  USB MSC: SCSI cmd 0x%02x failed: %d\n",
                cdb[0], rc);
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cbw));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)csw));
    return rc;
}

/* ── SCSI INQUIRY (0x12) ─────────────────────────────────────────────────── */
#define SCSI_INQUIRY_LEN 36

static int scsi_inquiry(uint8_t *vendor, size_t vendor_sz,
                        uint8_t *product, size_t product_sz)
{
    uint8_t *buf = (uint8_t *)alloc_dma(SCSI_INQUIRY_LEN);
    if (!buf) return -ENOMEM;

    uint8_t cdb[6] = {0x12, 0, 0, 0, SCSI_INQUIRY_LEN, 0};
    int rc = scsi_execute(cdb, 6, buf, SCSI_INQUIRY_LEN, 1);

    if (rc == 0 && vendor && vendor_sz > 0) {
        size_t vlen = vendor_sz - 1;
        if (vlen > 8) vlen = 8;
        memcpy(vendor, buf + 8, vlen);
        vendor[vlen] = '\0';
    }
    if (rc == 0 && product && product_sz > 0) {
        size_t plen = product_sz - 1;
        if (plen > 16) plen = 16;
        memcpy(product, buf + 16, plen);
        product[plen] = '\0';
    }

    if (rc == 0) {
        kprintf("  USB MSC: INQUIRY — %.8s %.16s rev=%.4s\n",
                (const char *)(buf + 8), (const char *)(buf + 16),
                (const char *)(buf + 32));
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return rc;
}

/* ── SCSI TEST UNIT READY (0x00) ─────────────────────────────────────────── */
static int scsi_test_unit_ready(void)
{
    uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
    return scsi_execute(cdb, 6, (void *)0, 0, 0);
}

/* ── SCSI REQUEST SENSE (0x03) ───────────────────────────────────────────── */
struct scsi_sense_data {
    uint8_t  response_code;
    uint8_t  _rsvd0;
    uint8_t  sense_key;
    uint8_t  info[4];
    uint8_t  additional_len;
    uint8_t  _rsvd1[4];
    uint8_t  asc;
    uint8_t  ascq;
    uint8_t  _rsvd2[4];
} __attribute__((packed));

static int scsi_request_sense(uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    struct scsi_sense_data *sense =
        (struct scsi_sense_data *)alloc_dma(sizeof(*sense));
    if (!sense) return -ENOMEM;

    uint8_t cdb[6] = {0x03, 0, 0, 0, sizeof(*sense), 0};
    int rc = scsi_execute(cdb, 6, sense, sizeof(*sense), 1);

    if (rc == 0) {
        if (sense_key) *sense_key = sense->sense_key & 0x0F;
        if (asc)       *asc       = sense->asc;
        if (ascq)      *ascq      = sense->ascq;
        kprintf("  USB MSC: SENSE key=0x%02x ASC=0x%02x ASCQ=0x%02x\n",
                sense->sense_key & 0x0F, sense->asc, sense->ascq);
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)sense));
    return rc;
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
    if (rc == 0) rc = bot_recv_csw(csw, cbw);

    if (rc == 0) {
        uint32_t lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                     | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
        *max_lba_out = lba;
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cbw));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)csw));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
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
    if (rc == 0) rc = bot_recv_csw(csw, cbw);

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cbw));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)csw));
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
    if (rc == 0) rc = bot_recv_csw(csw, cbw);

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cbw));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)csw));
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

/* ── Public init / exit ─────────────────────────────────────────── */

/*
 * Parse the configuration descriptor to find MSC interface with bulk endpoints.
 * Walks sub-descriptors searching for:
 *   - Interface descriptor with bInterfaceClass = 0x08 (MSC)
 *   - Bulk IN and Bulk OUT endpoint descriptors within that interface
 * Updates g_bulk_in_ep and g_bulk_out_ep on success.
 * Returns 0 on success, negative on error.
 */
static int usb_msc_parse_config(void)
{
    /* First get config descriptor header (9 bytes) to learn total length */
    uint8_t hdr[9];
    int rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR,
                         0x0200, 0, 9, hdr);
    if (rc < 0) {
        kprintf("  USB MSC: GET_CONFIG_DESC header failed (%d)\n", rc);
        return rc;
    }

    uint16_t total_len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
    if (total_len < 9 || total_len > 512) {
        kprintf("  USB MSC: invalid config descriptor length %u\n",
                (unsigned)total_len);
        return -EINVAL;
    }

    /* Allocate buffer for full config descriptor */
    uint8_t *config = (uint8_t *)alloc_dma((size_t)total_len);
    if (!config) return -ENOMEM;

    rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR,
                     0x0200, 0, total_len, config);
    if (rc < 0) {
        kprintf("  USB MSC: GET_CONFIG_DESC full failed (%d)\n", rc);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)config));
        return rc;
    }

    /* Walk sub-descriptors looking for MSC interface */
    int found = 0;
    uint16_t offset = config[0];  /* skip config descriptor header */
    while (offset + 1 < total_len) {
        uint8_t desc_len = config[offset];
        uint8_t desc_type = config[offset + 1];

        if (desc_len == 0) break;  /* malformed: prevent infinite loop */
        if (offset + desc_len > total_len) break;

        if (desc_type == USB_DT_INTERFACE && desc_len >= 9) {
            uint8_t if_class = config[offset + 5];
            if (if_class == 0x08) {  /* Mass Storage Class */
                /* Walk endpoints inside this interface */
                uint8_t num_ep = config[offset + 4];
                uint16_t ep_offset = offset + desc_len;
                int found_in = 0, found_out = 0;

                while (num_ep > 0 && ep_offset + 1 < total_len) {
                    uint8_t ep_len = config[ep_offset];
                    uint8_t ep_type = config[ep_offset + 1];

                    if (ep_len == 0) break;
                    if (ep_type == USB_DT_ENDPOINT && ep_len >= 7) {
                        uint8_t ep_addr = config[ep_offset + 2];
                        uint8_t ep_attr = config[ep_offset + 3];

                        if ((ep_attr & USB_ENDPOINT_XFERTYPE_MASK) ==
                            USB_ENDPOINT_XFER_BULK) {
                            if (ep_addr & USB_ENDPOINT_DIR_IN) {
                                g_bulk_in_ep = ep_addr;
                                found_in = 1;
                            } else {
                                g_bulk_out_ep = ep_addr;
                                found_out = 1;
                            }
                            num_ep--;
                        }
                    }
                    ep_offset += ep_len;
                }

                if (found_in && found_out) {
                    found = 1;
                    break;
                }
            }
        }

        offset = (uint16_t)(offset + desc_len);
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)config));

    if (!found) {
        kprintf("  USB MSC: no bulk endpoints found in config\n");
        return -ENODEV;
    }

    kprintf("  USB MSC: bulk IN=0x%02x bulk OUT=0x%02x\n",
            g_bulk_in_ep, g_bulk_out_ep);
    return 0;
}

int usb_msc_init(void)
{
    if (!usb_is_present()) return -1;
    if (usb_get_device_count() == 0) return -2;

    g_op_base = ehci_get_op_base();
    if (!g_op_base) return -3;

    /* Reset device state */
    g_bulk_in_ep  = 0x81;   /* default: EP1 IN */
    g_bulk_out_ep = 0x02;   /* default: EP2 OUT */
    g_tag = 1;
    g_max_lba = 0;

    /* SET_ADDRESS (standard SETUP to ep0, device address = 1) */
    int rc = usb_control(0x00, USB_REQ_SET_ADDRESS, g_dev_addr, 0, 0, (void *)0);
    if (rc < 0) {
        kprintf("  USB MSC: SET_ADDRESS failed (%d)\n", rc);
        return rc;
    }
    busy_wait(50000);

    /* GET_DESCRIPTOR — device descriptor (just to confirm it's alive) */
    uint8_t *desc = (uint8_t *)alloc_dma(18);
    if (!desc) return -ENOMEM;
    rc = usb_control(0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18, desc);
    if (rc < 0) {
        kprintf("  USB MSC: GET_DESCRIPTOR failed (%d)\n", rc);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
        return rc;
    }
    /* Validate device descriptor contents */
    if (desc[0] != 18) {
        kprintf("  USB MSC: invalid descriptor length (%u)\n", (unsigned)desc[0]);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
        return -EINVAL;
    }
    if (desc[1] != 0x01) {
        kprintf("  USB MSC: not a device descriptor (type 0x%02x)\n", desc[1]);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
        return -EINVAL;
    }
    uint8_t dev_class = desc[4] & 0xEF;
    if (dev_class != 0x00 && dev_class != 0x08) {
        kprintf("  USB MSC: unexpected device class 0x%02x\n", desc[4]);
        /* Non-fatal: some MSC devices report class 0 in the device descriptor
         * and set the interface descriptor class instead. Continue anyway. */
    }
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));

    /* SET_CONFIGURATION 1 */
    rc = usb_control(0x00, USB_REQ_SET_CONFIGURATION, 1, 0, 0, (void *)0);
    if (rc < 0) {
        kprintf("  USB MSC: SET_CONFIGURATION failed (%d)\n", rc);
        return rc;
    }

    /* Parse configuration descriptor to discover bulk endpoints */
    rc = usb_msc_parse_config();
    if (rc < 0) {
        kprintf("  USB MSC: endpoint discovery failed (%d), using defaults\n", rc);
        /* Non-fatal — fall back to hard-coded defaults */
    }

    /* BOT Class Reset */
    rc = bot_reset();
    if (rc < 0) {
        /* Non-fatal — some devices ignore it */
        kprintf("  USB MSC: BOT reset warning (%d)\n", rc);
    }

    /* Issue INQUIRY to identify the device */
    scsi_inquiry((uint8_t *)0, 0, (uint8_t *)0, 0);

    /* Wait for device ready (up to 5 retries) */
    for (int retry = 0; retry < 5; retry++) {
        rc = scsi_test_unit_ready();
        if (rc == 0) break;
        /* If check condition, request sense to clear */
        if (rc == -EIO) {
            scsi_request_sense((uint8_t *)0, (uint8_t *)0, (uint8_t *)0);
        }
        busy_wait(100000);
    }

    /* READ CAPACITY to discover drive size */
    rc = scsi_read_capacity(&g_max_lba);
    if (rc < 0) {
        kprintf("  USB MSC: READ CAPACITY failed (%d)\n", rc);
        return rc;
    }

    kprintf("  USB MSC: %lu sectors (%llu MB)\n",
            (unsigned long)(g_max_lba + 1),
            (unsigned long long)((g_max_lba + 1) / 2048));

    /* Register with block device layer */
    blockdev_register_legacy(BLOCKDEV_USB0, "usb0",
                      usb_msc_read_sectors,
                      usb_msc_write_sectors,
                      usb_msc_get_sectors);
    return 0;
}

/* Reverse usb_msc_init(): unregister block device and clear state */
void usb_msc_exit(void)
{
    blockdev_unregister(BLOCKDEV_USB0);
    g_op_base = 0;
    g_dev_addr = 1;
    g_max_lba = 0;
    kprintf("[USB] MSC device unregistered\n");
}

/* ── Public API: byte-level read/write/capacity (non-blockdev) ──── */

int usb_msc_read(void *dev, void *buf, size_t count, uint64_t offset)
{
    (void)dev;
    if (!buf || count == 0) return 0;

    /* Convert byte offset/count to LBA/sectors (512-byte sectors) */
    uint32_t lba = (uint32_t)(offset / 512);
    uint32_t nsec = (uint32_t)((count + 511) / 512);
    if (nsec > 255) nsec = 255;  /* limit to max sectors per request */

    return scsi_read10(lba, (uint8_t)nsec, buf);
}

int usb_msc_write(void *dev, const void *buf, size_t count, uint64_t offset)
{
    (void)dev;
    if (!buf || count == 0) return 0;

    uint32_t lba = (uint32_t)(offset / 512);
    uint32_t nsec = (uint32_t)((count + 511) / 512);
    if (nsec > 255) nsec = 255;

    return scsi_write10(lba, (uint8_t)nsec, buf);
}

int usb_msc_capacity(void *dev, void *cap)
{
    (void)dev;
    if (cap) {
        *(uint64_t *)cap = (uint64_t)(g_max_lba + 1);
    }
    return 0;
}
