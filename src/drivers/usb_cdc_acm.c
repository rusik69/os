/*
 * usb_cdc_acm.c — USB Communications Device Class (CDC) Abstract Control Model (ACM)
 *
 * Implements serial-over-USB for CDC ACM devices.  Registers as a TTY
 * device (ttyUSB0) using bulk IN/OUT endpoints for data transfer and
 * the control endpoint for line coding and control signals.
 *
 * References:
 *   USB Class Definitions for Communication Devices, Version 1.2
 *   PSTN Devices: Abstract Control Model, Version 1.2
 */

#include "usb.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "errno.h"
#include "module.h"

/* ── CDC ACM constants ─────────────────────────────────────────────── */

#define CDC_CLASS_COMM    0x02
#define CDC_SUBCLASS_ACM  0x02
#define CDC_PROTOCOL_V25TER 1

/* CDC descriptor subtypes */
#define CDC_HEADER        0x00
#define CDC_CM           0x01  /* Call Management */
#define CDC_ACM          0x02  /* Abstract Control Management */
#define CDC_UNION        0x06  /* Functional descriptors */
#define CDC_ETH          0x0F

/* ACM capabilities */
#define ACM_SUPPORT_LINE_CODING     (1U << 0)
#define ACM_SUPPORT_SEND_BREAK      (1U << 1)
#define ACM_SUPPORT_NET_CONNECTION  (1U << 2)

/* Requests */
#define CDC_SEND_ENCAPSULATED_COMMAND   0x00
#define CDC_GET_ENCAPSULATED_RESPONSE   0x01
#define CDC_SET_LINE_CODING             0x20
#define CDC_GET_LINE_CODING             0x21
#define CDC_SET_CONTROL_LINE_STATE      0x22
#define CDC_SEND_BREAK                  0x23

/* Serial state notification (from device) */
#define CDC_NOTIF_SERIAL_STATE          0x20
#define CDC_NOTIF_RESPONSE_AVAILABLE    0x01

/* Serial state bitmask */
#define CDC_SERIAL_STATE_OVERRUN        (1U << 0)
#define CDC_SERIAL_STATE_PARITY         (1U << 1)
#define CDC_SERIAL_STATE_FRAMING        (1U << 2)
#define CDC_SERIAL_STATE_RING           (1U << 3)
#define CDC_SERIAL_STATE_BREAK          (1U << 4)
#define CDC_SERIAL_STATE_DSR            (1U << 5)
#define CDC_SERIAL_STATE_DCD            (1U << 6)

/* Line coding structure */
struct cdc_line_coding {
    uint32_t dwDTERate;    /* data rate (baud) */
    uint8_t  bCharFormat;  /* 0=1 stop, 1=1.5 stop, 2=2 stop */
    uint8_t  bParityType;  /* 0=none, 1=odd, 2=even, 3=mark, 4=space */
    uint8_t  bDataBits;    /* 5, 6, 7, 8, 16 */
} __attribute__((packed));

/* Control line state */
#define LINE_STATE_DTR     (1U << 0)
#define LINE_STATE_RTS     (1U << 1)

/* ── EHCI transfer primitives (same layout as usb_msc.c) ────────────── */

#define MAX_PKT 512

struct ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
    uint32_t _pad[3];
} __attribute__((packed, aligned(32)));

struct ehci_qh {
    uint32_t next_qh;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t cur_qtd;
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t buf[5];
    uint32_t _pad[3];
} __attribute__((packed, aligned(32)));

#define QTD_STATUS_ACTIVE   (1u << 7)
#define QTD_STATUS_HALTED   (1u << 6)
#define QTD_STATUS_MASK     0xFFu
#define QTD_PID_OUT         (0u << 8)
#define QTD_PID_IN          (1u << 8)
#define QTD_PID_SETUP       (2u << 8)
#define QTD_IOC             (1u << 15)
#define QTD_CERR(x)         ((x) << 10)
#define QTD_BYTES(x)        ((uint32_t)(x) << 16)
#define QTD_DT              (1u << 31)

#define QH_DEVADDR(a)       ((a) & 0x7F)
#define QH_EP(n)            (((n) & 0xF) << 8)
#define QH_EPS_HS           (2u << 12)
#define QH_DTC              (1u << 14)
#define QH_H                (1u << 15)
#define QH_MAXPKT(n)        (((n) & 0x7FF) << 16)
#define QH_RL(n)            (((n) & 0xF) << 28)
#define QH_MULT(n)          (((n) & 3u) << 30)

#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CMD_ASE        (1u << 5)
#define EHCI_CMD_RUN        (1u << 0)
#define EHCI_STS_ASS        (1u << 15)

static uint64_t g_acm_op_base = 0;
static uint8_t  g_acm_dev_addr = 1;
static int      g_acm_initialized = 0;

/* Endpoint info */
static uint8_t g_bulk_in_ep  = 0;
static uint8_t g_bulk_out_ep = 0;

/* Serial data ring buffer */
#define ACM_BUF_SIZE 256
static uint8_t g_acm_rx_buf[ACM_BUF_SIZE];
static int g_acm_rx_head = 0;
static int g_acm_rx_tail = 0;

/* TTY device id */
static int g_tty_id = 0;

/* Current line coding state */
static struct cdc_line_coding g_line_coding = {
    .dwDTERate   = 115200,
    .bCharFormat = 0,  /* 1 stop bit */
    .bParityType = 0,  /* none */
    .bDataBits   = 8,
};

/* Current control line state */
static uint16_t g_control_line_state = 0;

/* Last known serial state from notifications */
static uint16_t g_serial_state = 0;

/* Interrupt IN endpoint for serial state notifications */
static uint8_t g_int_in_ep = 0;

/* Ack status endpoint phase buffer */
static uint8_t g_status_buf[8] __attribute__((aligned(32)));

/* ── DMA allocator ────────────────────────────────────────────────── */

static void *acm_alloc_dma(size_t sz) {
    (void)sz;
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return (void *)0;
    void *p = PHYS_TO_VIRT(frame);
    memset(p, 0, 4096);
    return p;
}

/* ── MMIO helpers ──────────────────────────────────────────────────── */

static inline uint32_t op_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_acm_op_base + off);
}
static inline void op_wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_acm_op_base + off) = val;
}
static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── EHCI transfer ─────────────────────────────────────────────────── */

static int ehci_do_transfer(uint32_t pid, uint8_t ep, void *data,
                             uint32_t len, int toggle) {
    if (!g_acm_op_base) return -1;

    struct ehci_qh  *qh  = (struct ehci_qh  *)acm_alloc_dma(sizeof(*qh));
    struct ehci_qtd *qtd = (struct ehci_qtd *)acm_alloc_dma(sizeof(*qtd));
    if (!qh || !qtd) {
        if (qh) pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
        if (qtd) pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
        return -2;
    }

    uint32_t token = QTD_STATUS_ACTIVE | QTD_CERR(3) | pid
                   | QTD_BYTES(len) | QTD_IOC;
    if (toggle) token |= QTD_DT;

    qtd->next     = 1;
    qtd->alt_next = 1;
    qtd->token    = token;
    qtd->buf[0]   = (uint32_t)VIRT_TO_PHYS(data);
    qtd->buf[1]   = (VIRT_TO_PHYS(data) & ~0xFFFu) + 0x1000u;
    qtd->buf[2]   = qtd->buf[1] + 0x1000u;
    qtd->buf[3]   = qtd->buf[2] + 0x1000u;
    qtd->buf[4]   = qtd->buf[3] + 0x1000u;

    uint32_t qh_phys = (uint32_t)VIRT_TO_PHYS(qh);
    qh->next_qh  = qh_phys | 0;
    qh->ep_char  = QH_DEVADDR(g_acm_dev_addr) | QH_EP(ep) | QH_EPS_HS
                 | QH_DTC | QH_H | QH_MAXPKT(MAX_PKT) | QH_RL(4);
    qh->ep_cap   = QH_MULT(1);
    qh->cur_qtd  = 0;
    qh->next_qtd = (uint32_t)VIRT_TO_PHYS(qtd);
    qh->alt_qtd  = 1;
    qh->token    = 0;

    uint32_t old_async = op_rd(EHCI_ASYNCLISTADDR);
    uint32_t old_cmd   = op_rd(EHCI_USBCMD);

    op_wr(EHCI_ASYNCLISTADDR, qh_phys);
    op_wr(EHCI_USBCMD, old_cmd | EHCI_CMD_ASE);

    int timeout = 200000;
    while (!(op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);
    if (!timeout) goto fail;

    timeout = 2000000;
    while ((qtd->token & QTD_STATUS_ACTIVE) && --timeout)
        busy_wait(10);

    op_wr(EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    timeout = 200000;
    while ((op_rd(EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);

    op_wr(EHCI_ASYNCLISTADDR, old_async);

    int rc = 0;
    if (qtd->token & QTD_STATUS_MASK & ~QTD_STATUS_ACTIVE)
        if (qtd->token & QTD_STATUS_HALTED) rc = -3;

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

/* USB control transfer */
static int usb_control(uint8_t bm_req_type, uint8_t b_req,
                        uint16_t w_val, uint16_t w_idx, uint16_t w_len,
                        void *data) {
    uint8_t *setup = (uint8_t *)acm_alloc_dma(8);
    if (!setup) return -1;
    setup[0] = bm_req_type;
    setup[1] = b_req;
    setup[2] = (uint8_t)(w_val & 0xFF);
    setup[3] = (uint8_t)(w_val >> 8);
    setup[4] = (uint8_t)(w_idx & 0xFF);
    setup[5] = (uint8_t)(w_idx >> 8);
    setup[6] = (uint8_t)(w_len & 0xFF);
    setup[7] = (uint8_t)(w_len >> 8);

    int rc = ehci_do_transfer(QTD_PID_SETUP, 0, setup, 8, 0);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)setup));
    if (rc < 0) return rc;

    if (w_len && data) {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_IN : QTD_PID_OUT);
        rc = ehci_do_transfer(pid, 0, data, w_len, 1);
        if (rc < 0) return rc;
    }

    {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_OUT : QTD_PID_IN);
        uint8_t *dummy = (uint8_t *)acm_alloc_dma(4);
        if (!dummy) return -1;
        rc = ehci_do_transfer(pid, 0, dummy, 0, 1);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)dummy));
    }
    return rc;
}

/* ── CDC ACM control requests ──────────────────────────────────────── */

static int cdc_set_line_coding(uint32_t baud, uint8_t data_bits,
                                uint8_t parity, uint8_t stop_bits) {
    struct cdc_line_coding *lc = (struct cdc_line_coding *)acm_alloc_dma(sizeof(*lc));
    if (!lc) return -1;
    lc->dwDTERate    = baud;
    lc->bCharFormat  = stop_bits;
    lc->bParityType  = parity;
    lc->bDataBits    = data_bits;

    int rc = usb_control(0x21, CDC_SET_LINE_CODING, 0, 0,
                          sizeof(*lc), lc);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)lc));
    return rc;
}

static int cdc_set_control_line_state(uint16_t state) {
    g_control_line_state = state;
    return usb_control(0x21, CDC_SET_CONTROL_LINE_STATE, state, 0, 0, NULL);
}

static int cdc_get_line_coding(struct cdc_line_coding *lc) {
    if (!lc) return -EINVAL;
    struct cdc_line_coding *buf = (struct cdc_line_coding *)acm_alloc_dma(sizeof(*buf));
    if (!buf) return -ENOMEM;
    memset(buf, 0, sizeof(*buf));

    int rc = usb_control(0xA1, CDC_GET_LINE_CODING, 0, 0,
                          sizeof(*buf), buf);
    if (rc >= 0) {
        lc->dwDTERate   = buf->dwDTERate;
        lc->bCharFormat = buf->bCharFormat;
        lc->bParityType = buf->bParityType;
        lc->bDataBits   = buf->bDataBits;
    }
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return (rc < 0) ? rc : 0;
}

static int cdc_send_break(uint16_t duration) {
    return usb_control(0x21, CDC_SEND_BREAK, duration, 0, 0, NULL);
}

/* ── Serial data transfer ──────────────────────────────────────────── */

static int acm_read_data(uint8_t *buf, uint32_t len) {
    return ehci_do_transfer(QTD_PID_IN, g_bulk_in_ep, buf, len, 1);
}

/* ── Serial state notification ───────────────────────────────────── */

/*
 * USB CDC ACM notification structure (10 bytes on interrupt IN EP).
 * bmRequestType: 0xA1 (Class, IN, Interface)
 * bNotification: 0x20 (SERIAL_STATE)
 * wValue:        0
 * wIndex:        Interface number
 * wLength:       2
 * Data:          2-byte serial state bitmask
 */
struct __attribute__((packed)) cdc_notification {
    uint8_t  bmRequestType;
    uint8_t  bNotification;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint16_t wData;
};

/*
 * Poll the interrupt IN endpoint for a serial state notification.
 * Returns 0 on success (notification parsed), negative errno on failure.
 */
static int acm_poll_notification(void) {
    if (!g_int_in_ep) return 0;

    struct cdc_notification *notif;
    notif = (struct cdc_notification *)acm_alloc_dma(sizeof(*notif));
    if (!notif) return -ENOMEM;

    memset(notif, 0, sizeof(*notif));

    int rc = ehci_do_transfer(QTD_PID_IN, g_int_in_ep,
                               (uint8_t *)notif, sizeof(*notif), 1);
    if (rc == 0 && notif->bmRequestType == 0xA1 &&
        notif->bNotification == CDC_NOTIF_SERIAL_STATE) {
        g_serial_state = notif->wData;
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)notif));
    return (rc < 0) ? rc : 0;
}

/* ── Initialisation ────────────────────────────────────────────────── */

int __init usb_cdc_acm_init(void) {
    if (g_acm_initialized) return 0;
    if (!usb_is_present()) return -1;

    g_acm_op_base = ehci_get_op_base();
    if (!g_acm_op_base) return -2;

    /* SET_ADDRESS */
    int rc = usb_control(0x00, 0x05, g_acm_dev_addr, 0, 0, NULL);
    if (rc < 0) {
        kprintf("[USB ACM] SET_ADDRESS failed (%d)\n", rc);
        return rc;
    }
    busy_wait(50000);

    /* GET_DESCRIPTOR (config) to find CDC ACM interfaces */
    uint8_t *cfg = (uint8_t *)acm_alloc_dma(256);
    if (!cfg) return -3;
    rc = usb_control(0x80, 0x06, 0x0200, 0, 256, cfg);
    if (rc < 0) {
        kprintf("[USB ACM] GET_DESCRIPTOR(config) failed\n");
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));
        return rc;
    }

    int cfg_len = cfg[0];
    int pos = 0;
    int cdc_found = 0;
    int data_iface_num = -1;

    while (pos < cfg_len) {
        uint8_t dlen = cfg[pos];
        uint8_t dtype = cfg[pos + 1];
        if (dlen == 0) break;

        if (dtype == 4) {  /* Interface */
            uint8_t if_class  = cfg[pos + 5];
            uint8_t if_sub    = cfg[pos + 6];
            uint8_t if_proto  = cfg[pos + 7];

            if (if_class == CDC_CLASS_COMM && if_sub == CDC_SUBCLASS_ACM) {
                cdc_found = 1;
                kprintf("[USB ACM] Found CDC ACM interface (proto=0x%02x)\n", if_proto);
            }
            if (if_class == 0x0A) {  /* CDC Data */
                data_iface_num = cfg[pos + 2];
                kprintf("[USB ACM] Found CDC Data interface %d\n", data_iface_num);
            }
        } else if (dtype == 5) {  /* Endpoint */
            uint8_t ep_addr = cfg[pos + 2];
            uint8_t ep_attr = cfg[pos + 3];

            if ((ep_attr & 0x03) == 2) {  /* Bulk endpoint */
                if (ep_addr & 0x80) {
                    g_bulk_in_ep = ep_addr & 0x0F;
                    kprintf("[USB ACM] Bulk IN ep=0x%02x\n", ep_addr);
                } else {
                    g_bulk_out_ep = ep_addr & 0x0F;
                    kprintf("[USB ACM] Bulk OUT ep=0x%02x\n", ep_addr);
                }
            } else if ((ep_attr & 0x03) == 3 && (ep_addr & 0x80)) {
                /* Interrupt IN endpoint — used for serial state notifications */
                g_int_in_ep = ep_addr & 0x0F;
                kprintf("[USB ACM] Interrupt IN ep=0x%02x\n", ep_addr);
            }
        }

        pos += dlen;
    }

    if (!cdc_found) {
        kprintf("[USB ACM] No CDC ACM device found\n");
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));
        return -4;
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));

    if (!g_bulk_in_ep || !g_bulk_out_ep) {
        kprintf("[USB ACM] Missing bulk endpoints\n");
        return -5;
    }

    /* SET_CONFIGURATION 1 */
    rc = usb_control(0x00, 0x09, 1, 0, 0, NULL);
    if (rc < 0) {
        kprintf("[USB ACM] SET_CONFIGURATION failed (%d)\n", rc);
        return rc;
    }

    /* Configure line coding: 115200 8N1 */
    rc = cdc_set_line_coding(115200, 8, 0, 0);
    if (rc < 0) {
        kprintf("[USB ACM] SET_LINE_CODING failed (%d)\n", rc);
    }

    /* Read back line coding to verify and store in state */
    {
        struct cdc_line_coding lc_check;
        rc = cdc_get_line_coding(&lc_check);
        if (rc == 0) {
            g_line_coding = lc_check;
            kprintf("[USB ACM] Line coding: %u baud, %u data bits, "
                    "parity=%u, stop=%u\n",
                    (unsigned)lc_check.dwDTERate,
                    (unsigned)lc_check.bDataBits,
                    (unsigned)lc_check.bParityType,
                    (unsigned)lc_check.bCharFormat);
        }
    }

    /* Set control line state: DTR + RTS */
    rc = cdc_set_control_line_state(LINE_STATE_DTR | LINE_STATE_RTS);
    if (rc < 0) {
        kprintf("[USB ACM] SET_CONTROL_LINE_STATE failed (%d)\n", rc);
    }

    /* Initial poll for serial state notification */
    if (g_int_in_ep) {
        acm_poll_notification();
        kprintf("[USB ACM] Serial state: 0x%04x\n",
                (unsigned)g_serial_state);
    }

    g_acm_initialized = 1;
    g_tty_id = 0;  /* ttyUSB0 */

    kprintf("[USB ACM] Initialized as ttyUSB%d (115200 8N1)\n", g_tty_id);
    return 0;
}

/* ── Poll for incoming serial data ─────────────────────────────────── */

void usb_cdc_acm_poll(void) {
    if (!g_acm_initialized) return;

    /* Poll interrupt endpoint for serial state notifications */
    acm_poll_notification();

    uint8_t *buf = (uint8_t *)acm_alloc_dma(64);
    if (!buf) return;

    int rc = acm_read_data(buf, 64);
    if (rc == 0) {
        /* Copy received data into ring buffer */
        for (int i = 0; i < 64; i++) {
            if (buf[i] == 0) break;  /* short packet */
            int next = (g_acm_rx_tail + 1) % ACM_BUF_SIZE;
            if (next != g_acm_rx_head) {
                g_acm_rx_buf[g_acm_rx_tail] = buf[i];
                g_acm_rx_tail = next;
            }
        }
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
}

/* ── Public API ────────────────────────────────────────────────────── */

int usb_cdc_acm_write(const uint8_t *data, int len) {
    if (!g_acm_initialized) return -ENODEV;
    if (!data || len <= 0) return -EINVAL;
    return ehci_do_transfer(QTD_PID_OUT, g_bulk_out_ep, (void *)data,
                             (uint32_t)len, 0);
}

int usb_cdc_acm_read(uint8_t *buf, int maxlen) {
    if (!g_acm_initialized) return -ENODEV;
    if (!buf || maxlen <= 0) return -EINVAL;
    int count = 0;
    while (count < maxlen && g_acm_rx_head != g_acm_rx_tail) {
        buf[count++] = g_acm_rx_buf[g_acm_rx_head];
        g_acm_rx_head = (g_acm_rx_head + 1) % ACM_BUF_SIZE;
    }
    return count;
}

int usb_cdc_acm_available(void) {
    if (!g_acm_initialized) return 0;
    return (g_acm_rx_tail - g_acm_rx_head + ACM_BUF_SIZE) % ACM_BUF_SIZE;
}

/* ── Control line management API ──────────────────────────────────── */

int usb_cdc_acm_set_line_coding(uint32_t baud, uint8_t data_bits,
                                 uint8_t parity, uint8_t stop_bits) {
    if (!g_acm_initialized) return -ENODEV;
    struct cdc_line_coding lc;
    lc.dwDTERate   = baud;
    lc.bCharFormat = stop_bits;
    lc.bParityType = parity;
    lc.bDataBits   = data_bits;

    struct cdc_line_coding *buf;
    buf = (struct cdc_line_coding *)acm_alloc_dma(sizeof(*buf));
    if (!buf) return -ENOMEM;
    memcpy(buf, &lc, sizeof(lc));

    int rc = usb_control(0x21, CDC_SET_LINE_CODING, 0, 0,
                          sizeof(*buf), buf);
    if (rc >= 0)
        g_line_coding = lc;

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return (rc < 0) ? rc : 0;
}

int usb_cdc_acm_get_line_coding(uint32_t *baud, uint8_t *data_bits,
                                 uint8_t *parity, uint8_t *stop_bits) {
    if (!g_acm_initialized) return -ENODEV;
    if (!baud || !data_bits || !parity || !stop_bits) return -EINVAL;

    struct cdc_line_coding lc;
    int rc = cdc_get_line_coding(&lc);
    if (rc < 0) return rc;

    *baud      = lc.dwDTERate;
    *data_bits = lc.bDataBits;
    *parity    = lc.bParityType;
    *stop_bits = lc.bCharFormat;
    return 0;
}

int usb_cdc_acm_set_control_line_state(uint16_t state) {
    if (!g_acm_initialized) return -ENODEV;
    return cdc_set_control_line_state(state);
}

int usb_cdc_acm_get_control_line_state(uint16_t *state) {
    if (!g_acm_initialized) return -ENODEV;
    if (!state) return -EINVAL;
    *state = g_control_line_state;
    return 0;
}

int usb_cdc_acm_send_break(uint16_t duration) {
    if (!g_acm_initialized) return -ENODEV;
    return cdc_send_break(duration);
}

int usb_cdc_acm_get_serial_state(uint16_t *state) {
    if (!g_acm_initialized) return -ENODEV;
    if (!state) return -EINVAL;
    *state = g_serial_state;
    return 0;
}

/* ── cdc_acm generic driver interface (wraps usb_cdc_acm_* API) ──── */

int cdc_acm_init(void *dev) {
    (void)dev;
    if (g_acm_initialized) return 0;
    return usb_cdc_acm_init();
}

int cdc_acm_open(void *dev) {
    (void)dev;
    if (!g_acm_initialized) return -ENODEV;
    /* Assert DTR + RTS on open */
    return cdc_set_control_line_state(LINE_STATE_DTR | LINE_STATE_RTS);
}

int cdc_acm_close(void *dev) {
    (void)dev;
    if (!g_acm_initialized) return -ENODEV;
    /* De-assert DTR + RTS on close */
    return cdc_set_control_line_state(0);
}

int cdc_acm_write(void *dev, const void *buf, size_t count) {
    (void)dev;
    return usb_cdc_acm_write((const uint8_t *)buf, (int)count);
}

int cdc_acm_read(void *dev, void *buf, size_t count) {
    (void)dev;
    return usb_cdc_acm_read((uint8_t *)buf, (int)count);
}

/* ── Exit / cleanup ────────────────────────────────────────────────── */

void usb_cdc_acm_exit(void) {
    if (!g_acm_initialized) return;

    /* De-assert control lines */
    cdc_set_control_line_state(0);

    g_acm_initialized = 0;
    g_acm_dev_addr = 0;
    g_bulk_in_ep = 0;
    g_bulk_out_ep = 0;
    g_int_in_ep = 0;
    g_acm_rx_head = 0;
    g_acm_rx_tail = 0;
    g_tty_id = 0;
    g_control_line_state = 0;
    g_serial_state = 0;

    kprintf("[USB ACM] CDC ACM driver shut down\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("USB CDC ACM serial driver with control line management");
MODULE_AUTHOR("Hermes 1000 Changes");
