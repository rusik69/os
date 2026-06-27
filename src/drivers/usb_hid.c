/*
 * usb_hid.c — USB HID Boot Protocol driver
 *
 * Interfaces with HID keyboards and mice using the boot protocol
 * on interrupt endpoints.  Provides keyboard input (via keyboard
 * API) and mouse input APIs.
 *
 * References:
 *   USB HID Specification, Version 1.11
 *   USB Device Class Definition for HID, Version 1.11
 */

#include "uhid.h"
#include "usb.h"
#include "errno.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

/* ── EHCI MMIO / transfer primitives (adapted from usb_msc.c) ───────────── */

#define MAX_PKT 64   /* interrupt endpoint max packet */

/* qTD token field bits (must match usb_msc.c defines) */
#define QTD_STATUS_ACTIVE   (1u << 7)
#define QTD_STATUS_HALTED   (1u << 6)
#define QTD_STATUS_MASK     0xFFu
#define QTD_PID_OUT         (0u << 8)

/* Allocate DMA buffer (4 KB page) */
static void *hid_alloc_dma(size_t sz) {
    (void)sz;
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return (void *)0;
    void *p = PHYS_TO_VIRT(frame);
    memset(p, 0, 4096);
    return p;
}

/* EHCI MMIO helpers */
static inline uint32_t op_rd(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void op_wr(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* qTD and qH structures (same layout as EHCI spec) */
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

/* ── HID driver state ──────────────────────────────────────────────── */

static uint64_t g_op_base = 0;
static uint8_t  g_dev_addr = 1;
static int      g_hid_initialized = 0;

/* HID device info */
static int g_keyboard_present = 0;
static int g_mouse_present = 0;
static int g_has_interrupt = 0;

/* Interrupt endpoint info */
static uint8_t g_int_in_ep = 0;
static uint8_t g_mouse_int_in_ep = 0;

/* Keyboard state */
static struct hid_keyboard_report g_last_kbd_report;
static int g_kbd_changed = 0;
static char g_kbd_buf[16];
static int g_kbd_buf_head = 0;
static int g_kbd_buf_tail = 0;

/* Mouse state */
static int g_mouse_buttons = 0;
static int g_mouse_dx = 0;
static int g_mouse_dy = 0;

/* ── Transfer primitives ───────────────────────────────────────────── */

static int ehci_do_transfer(uint64_t op_base, uint32_t pid, uint8_t ep,
                             void *data, uint32_t len, int toggle,
                             uint8_t dev_addr) {
    if (!op_base) return -1;

    struct ehci_qh  *qh  = (struct ehci_qh  *)hid_alloc_dma(sizeof(*qh));
    struct ehci_qtd *qtd = (struct ehci_qtd *)hid_alloc_dma(sizeof(*qtd));
    if (!qh || !qtd) {
        if (qh) pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
        if (qtd) pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
        return -2;
    }

    uint32_t token = QTD_STATUS_ACTIVE
                   | QTD_CERR(3)
                   | pid
                   | QTD_BYTES(len)
                   | QTD_IOC;
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
    qh->ep_char  = QH_DEVADDR(dev_addr)
                 | QH_EP(ep)
                 | QH_EPS_HS
                 | QH_DTC
                 | QH_H
                 | QH_MAXPKT(MAX_PKT)
                 | QH_RL(4);
    qh->ep_cap   = QH_MULT(1);
    qh->cur_qtd  = 0;
    qh->next_qtd = (uint32_t)VIRT_TO_PHYS(qtd);
    qh->alt_qtd  = 1;
    qh->token    = 0;

    uint32_t old_async = op_rd(op_base, EHCI_ASYNCLISTADDR);
    uint32_t old_cmd   = op_rd(op_base, EHCI_USBCMD);

    op_wr(op_base, EHCI_ASYNCLISTADDR, qh_phys);
    op_wr(op_base, EHCI_USBCMD, old_cmd | EHCI_CMD_ASE);

    int timeout = 200000;
    while (!(op_rd(op_base, EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);
    if (!timeout) goto fail;

    timeout = 2000000;
    while ((qtd->token & QTD_STATUS_ACTIVE) && --timeout)
        busy_wait(10);

    op_wr(op_base, EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    timeout = 200000;
    while ((op_rd(op_base, EHCI_USBSTS) & EHCI_STS_ASS) && --timeout)
        busy_wait(10);

    op_wr(op_base, EHCI_ASYNCLISTADDR, old_async);

    int rc = 0;
    if (qtd->token & 0xFF & ~QTD_STATUS_ACTIVE)
        if (qtd->token & QTD_STATUS_HALTED) rc = -3;

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
    return rc;

fail:
    op_wr(op_base, EHCI_USBCMD, old_cmd & ~EHCI_CMD_ASE);
    op_wr(op_base, EHCI_ASYNCLISTADDR, old_async);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qtd));
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)qh));
    return -4;
}

/* USB control transfer */
static int usb_control(uint64_t op_base, uint8_t bm_req_type, uint8_t b_req,
                        uint16_t w_val, uint16_t w_idx, uint16_t w_len,
                        void *data, uint8_t dev_addr) {
    uint8_t *setup = (uint8_t *)hid_alloc_dma(8);
    if (!setup) return -1;
    setup[0] = bm_req_type;
    setup[1] = b_req;
    setup[2] = (uint8_t)(w_val & 0xFF);
    setup[3] = (uint8_t)(w_val >> 8);
    setup[4] = (uint8_t)(w_idx & 0xFF);
    setup[5] = (uint8_t)(w_idx >> 8);
    setup[6] = (uint8_t)(w_len & 0xFF);
    setup[7] = (uint8_t)(w_len >> 8);

    int rc = ehci_do_transfer(op_base, QTD_PID_SETUP, 0, setup, 8, 0, dev_addr);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)setup));
    if (rc < 0) return rc;

    if (w_len && data) {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_IN : QTD_PID_OUT);
        rc = ehci_do_transfer(op_base, pid, 0, data, w_len, 1, dev_addr);
        if (rc < 0) return rc;
    }

    {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_OUT : QTD_PID_IN);
        uint8_t *dummy = (uint8_t *)hid_alloc_dma(4);
        if (!dummy) return -1;
        rc = ehci_do_transfer(op_base, pid, 0, dummy, 0, 1, dev_addr);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)dummy));
    }
    return rc;
}

/* ── HID keycode to ASCII mapping (US layout, boot protocol subset) ── */

static const char hid_keycode_to_ascii[128] = {
    [HID_KEYCODE_NONE]     = 0,
    [HID_KEYCODE_A]        = 'a', [HID_KEYCODE_B] = 'b', [HID_KEYCODE_C] = 'c',
    [HID_KEYCODE_D]        = 'd', [HID_KEYCODE_E] = 'e', [HID_KEYCODE_F] = 'f',
    [HID_KEYCODE_G]        = 'g', [HID_KEYCODE_H] = 'h', [HID_KEYCODE_I] = 'i',
    [HID_KEYCODE_J]        = 'j', [HID_KEYCODE_K] = 'k', [HID_KEYCODE_L] = 'l',
    [HID_KEYCODE_M]        = 'm', [HID_KEYCODE_N] = 'n', [HID_KEYCODE_O] = 'o',
    [HID_KEYCODE_P]        = 'p', [HID_KEYCODE_Q] = 'q', [HID_KEYCODE_R] = 'r',
    [HID_KEYCODE_S]        = 's', [HID_KEYCODE_T] = 't', [HID_KEYCODE_U] = 'u',
    [HID_KEYCODE_V]        = 'v', [HID_KEYCODE_W] = 'w', [HID_KEYCODE_X] = 'x',
    [HID_KEYCODE_Y]        = 'y', [HID_KEYCODE_Z] = 'z',
    [HID_KEYCODE_1]        = '1', [HID_KEYCODE_2] = '2', [HID_KEYCODE_3] = '3',
    [HID_KEYCODE_4]        = '4', [HID_KEYCODE_5] = '5', [HID_KEYCODE_6] = '6',
    [HID_KEYCODE_7]        = '7', [HID_KEYCODE_8] = '8', [HID_KEYCODE_9] = '9',
    [HID_KEYCODE_0]        = '0',
    [HID_KEYCODE_ENTER]    = '\n',
    [HID_KEYCODE_ESC]      = 0x1B,
    [HID_KEYCODE_BACKSPACE]= '\b',
    [HID_KEYCODE_TAB]      = '\t',
    [HID_KEYCODE_SPACE]    = ' ',
    [HID_KEYCODE_MINUS]    = '-', [HID_KEYCODE_EQUAL]   = '=',
    [HID_KEYCODE_LBRACKET] = '[', [HID_KEYCODE_RBRACKET]= ']',
    [HID_KEYCODE_BSLASH]   = '\\',
    [HID_KEYCODE_SEMICOLON]= ';', [HID_KEYCODE_QUOTE]  = '\'',
    [HID_KEYCODE_GRAVE]    = '`',
    [HID_KEYCODE_COMMA]    = ',', [HID_KEYCODE_DOT]     = '.',
    [HID_KEYCODE_SLASH]    = '/',
};

static const char hid_keycode_shifted[128] = {
    [HID_KEYCODE_1]        = '!', [HID_KEYCODE_2] = '@', [HID_KEYCODE_3] = '#',
    [HID_KEYCODE_4]        = '$', [HID_KEYCODE_5] = '%', [HID_KEYCODE_6] = '^',
    [HID_KEYCODE_7]        = '&', [HID_KEYCODE_8] = '*', [HID_KEYCODE_9] = '(',
    [HID_KEYCODE_0]        = ')',
    [HID_KEYCODE_MINUS]    = '_', [HID_KEYCODE_EQUAL]   = '+',
    [HID_KEYCODE_LBRACKET] = '{', [HID_KEYCODE_RBRACKET]= '}',
    [HID_KEYCODE_BSLASH]   = '|',
    [HID_KEYCODE_SEMICOLON]= ':', [HID_KEYCODE_QUOTE]  = '"',
    [HID_KEYCODE_GRAVE]    = '~',
    [HID_KEYCODE_COMMA]    = '<', [HID_KEYCODE_DOT]     = '>',
    [HID_KEYCODE_SLASH]    = '?',
};

/* Modifier key masks */
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RGUI   0x80

/* Convert HID keycode + modifiers to ASCII */
static int hid_key_to_ascii(uint8_t keycode, uint8_t modifiers) {
    if (keycode == HID_KEYCODE_NONE) return 0;

    int shifted = (modifiers & (MOD_LSHIFT | MOD_RSHIFT)) != 0;
    int ctrl = (modifiers & (MOD_LCTRL | MOD_RCTRL)) != 0;

    /* Letter keys: caps lock shifts letters */
    if (keycode >= HID_KEYCODE_A && keycode <= HID_KEYCODE_Z) {
        char c = hid_keycode_to_ascii[keycode];
        if (shifted)
            c = c - 'a' + 'A';
        if (ctrl)
            c = c - 'a' + 1;  /* Ctrl+A = 1, etc. */
        return c;
    }

    /* Number/symbol row */
    if (shifted && hid_keycode_shifted[keycode])
        return hid_keycode_shifted[keycode];
    if (!shifted && hid_keycode_to_ascii[keycode])
        return hid_keycode_to_ascii[keycode];

    /* Special keys via 0x80 prefix */
    if (keycode == HID_KEYCODE_UP)    return 0x80 | 'A';  /* KEY_UP */
    if (keycode == HID_KEYCODE_DOWN)  return 0x80 | 'B';  /* KEY_DOWN */
    if (keycode == HID_KEYCODE_LEFT)  return 0x80 | 'D';  /* KEY_LEFT */
    if (keycode == HID_KEYCODE_RIGHT) return 0x80 | 'C';  /* KEY_RIGHT */

    return 0;
}

/* ── Keyboard report processing ────────────────────────────────────── */

static void process_keyboard_report(const struct hid_keyboard_report *rep) {
    /* Compare with last report to detect changes */
    /* For simplicity, just process the first non-zero keycode */
    for (int i = 0; i < 6; i++) {
        uint8_t kc = rep->keys[i];
        if (kc != HID_KEYCODE_NONE) {
            int ch = hid_key_to_ascii(kc, rep->modifiers);
            if (ch > 0) {
                int next = (g_kbd_buf_tail + 1) % sizeof(g_kbd_buf);
                if (next != g_kbd_buf_head) {
                    g_kbd_buf[g_kbd_buf_tail] = (char)ch;
                    g_kbd_buf_tail = next;
                }
            }
        }
    }
    memcpy(&g_last_kbd_report, rep, sizeof(g_last_kbd_report));
    g_kbd_changed = 1;
}

/* ── Mouse report processing ───────────────────────────────────────── */

static void process_mouse_report(const struct hid_mouse_report *rep) {
    g_mouse_buttons = rep->buttons;
    g_mouse_dx += rep->x_delta;
    g_mouse_dy += rep->y_delta;
}

/* ── Poll HID interrupt endpoint ────────────────────────────────────── */

static void hid_poll_interrupt_in(void) {
    if (!g_int_in_ep) return;

    /* Allocate buffer for keyboard report */
    uint8_t *buf = (uint8_t *)hid_alloc_dma(64);
    if (!buf) return;

    int rc = ehci_do_transfer(g_op_base, QTD_PID_IN, g_int_in_ep,
                               buf, 8, 0, g_dev_addr);
    if (rc == 0) {
        process_keyboard_report((const struct hid_keyboard_report *)buf);
    }
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));

    /* Poll mouse interrupt endpoint separately if present */
    if (g_mouse_int_in_ep) {
        uint8_t *mbuf = (uint8_t *)hid_alloc_dma(64);
        if (!mbuf) return;
        rc = ehci_do_transfer(g_op_base, QTD_PID_IN, g_mouse_int_in_ep,
                               mbuf, 4, 0, g_dev_addr);
        if (rc == 0) {
            process_mouse_report((const struct hid_mouse_report *)mbuf);
        }
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)mbuf));
    }
}

/* ── Initialisation ────────────────────────────────────────────────── */

int usb_hid_init(void) {
    if (g_hid_initialized) return 0;
    if (!usb_is_present()) return -1;

    /* Get EHCI operational base */
    g_op_base = ehci_get_op_base();
    if (!g_op_base) return -2;

    /* Default device address (assigned by EHCI reset) */
    g_dev_addr = 1;

    /* SET_ADDRESS */
    int rc = usb_control(g_op_base, 0x00, 0x05, g_dev_addr, 0, 0, NULL, 0);
    if (rc < 0) {
        kprintf("[USB HID] SET_ADDRESS failed (%d)\n", rc);
        return rc;
    }
    busy_wait(50000);

    /* GET_DESCRIPTOR (device) */
    uint8_t *desc = (uint8_t *)hid_alloc_dma(18);
    if (!desc) return -3;
    rc = usb_control(g_op_base, 0x80, 0x06, 0x0100, 0, 18, desc, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] GET_DESCRIPTOR(device) failed\n");
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
        return rc;
    }
    uint8_t dev_class = desc[4];
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));

    /* GET_DESCRIPTOR (config) to find HID interfaces */
    uint8_t *cfg = (uint8_t *)hid_alloc_dma(256);
    if (!cfg) return -4;
    rc = usb_control(g_op_base, 0x80, 0x06, 0x0200, 0, 256, cfg, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] GET_DESCRIPTOR(config) failed\n");
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));
        return rc;
    }

    /* Parse configuration descriptor for HID interfaces */
    int cfg_len = cfg[0];
    int pos = 0;
    while (pos + 1 < cfg_len) {
        uint8_t dlen = cfg[pos];
        uint8_t dtype = cfg[pos + 1];
        if (dlen < 2) break;                /* minimum descriptor size */
        if (pos + dlen > cfg_len) break;    /* descriptor extends past buffer */

        if (dtype == 4 && pos + 8 <= cfg_len) {
            /* Interface descriptor */
            uint8_t if_class  = cfg[pos + 5];
            uint8_t if_sub    = cfg[pos + 6];
            uint8_t if_proto  = cfg[pos + 7];
            uint8_t if_num    = cfg[pos + 2];

            if (if_class == 0x03) {  /* HID */
                if (if_proto == 1) {  /* Keyboard */
                    g_keyboard_present = 1;
                    kprintf("[USB HID] Found keyboard (if=%d)\n", if_num);
                } else if (if_proto == 2) {  /* Mouse */
                    g_mouse_present = 1;
                    kprintf("[USB HID] Found mouse (if=%d)\n", if_num);
                }
            }
        } else if (dtype == 5 && pos + 6 <= cfg_len) {
            /* Endpoint descriptor */
            uint8_t ep_addr  = cfg[pos + 2];
            uint8_t ep_attr  = cfg[pos + 3];

            if ((ep_attr & 0x03) == 3) {  /* Interrupt endpoint */
                if (ep_addr & 0x80) {  /* IN endpoint */
                    if (g_keyboard_present && !g_int_in_ep) {
                        g_int_in_ep = ep_addr & 0x0F;
                        kprintf("[USB HID] Kbd int IN ep=0x%02x\n", ep_addr);
                    } else if (g_mouse_present && !g_mouse_int_in_ep) {
                        g_mouse_int_in_ep = ep_addr & 0x0F;
                        kprintf("[USB HID] Mouse int IN ep=0x%02x\n", ep_addr);
                    }
                }
            }
        }

        pos += dlen;
    }

    if (!g_keyboard_present && !g_mouse_present) {
        kprintf("[USB HID] No HID devices found\n");
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));
        return -5;
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)cfg));

    /* SET_CONFIGURATION 1 */
    rc = usb_control(g_op_base, 0x00, 0x09, 1, 0, 0, NULL, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] SET_CONFIGURATION failed (%d)\n", rc);
        return rc;
    }

    /* SET_PROTOCOL to boot protocol */
    rc = usb_control(g_op_base, 0x21, 0x0B, 0, 0, 0, NULL, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] SET_PROTOCOL warning (%d)\n", rc);
    }

    /* SET_IDLE to 0 (no rate limit) */
    rc = usb_control(g_op_base, 0x21, 0x0A, 0, 0, 0, NULL, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] SET_IDLE warning (%d)\n", rc);
    }

    g_hid_initialized = 1;
    kprintf("[USB HID] Initialized: %s%s\n",
            g_keyboard_present ? "keyboard " : "",
            g_mouse_present ? "mouse" : "");
    return 0;
}

/* ── Poll HID devices ──────────────────────────────────────────────── */

void usb_hid_poll(void) {
    if (!g_hid_initialized || !g_has_interrupt) return;
    hid_poll_interrupt_in();
}

/* ── Public API ────────────────────────────────────────────────────── */

int usb_hid_keyboard_present(void) {
    return g_keyboard_present;
}

int usb_hid_getchar(void) {
    if (g_kbd_buf_head == g_kbd_buf_tail) return -1;
    char c = g_kbd_buf[g_kbd_buf_head];
    g_kbd_buf_head = (g_kbd_buf_head + 1) % sizeof(g_kbd_buf);
    return c;
}

int usb_hid_has_input(void) {
    return g_kbd_buf_head != g_kbd_buf_tail;
}

int usb_hid_mouse_present(void) {
    return g_mouse_present;
}

void usb_hid_mouse_get(int *buttons, int *dx, int *dy) {
    if (buttons) *buttons = g_mouse_buttons;
    if (dx) *dx = g_mouse_dx;
    if (dy) *dy = g_mouse_dy;
    g_mouse_dx = 0;
    g_mouse_dy = 0;
}

int usb_hid_read(struct usb_device *dev, void *buf, size_t count)
{
    if (!dev || !buf || count == 0) return -EINVAL;

    /* Use interrupt IN transfer to read HID report */
    if (!g_hid_initialized || !g_dev_addr) {
        kprintf("[usb_hid] usb_hid_read: HID not initialized\n");
        return -EIO;
    }

    /* For keyboard, return scancodes from internal buffer */
    if (g_keyboard_present) {
        int n = 0;
        while (n < (int)count && usb_hid_has_input()) {
            ((uint8_t *)buf)[n++] = (uint8_t)usb_hid_getchar();
        }
        return n;
    }

    /* For mouse, return current state */
    if (g_mouse_present) {
        if (count >= 7) {
            uint8_t *p = (uint8_t *)buf;
            int buttons, dx, dy;
            usb_hid_mouse_get(&buttons, &dx, &dy);
            p[0] = (uint8_t)buttons;
            p[1] = (uint8_t)dx;
            p[2] = (uint8_t)dy;
            /* Clear state */
            memset(p + 3, 0, count - 3);
            return 3;
        }
    }

    return 0;
}

/* ── usb_hid_parse_report: Parse HID report descriptor ────────── */
int usb_hid_parse_report(void *dev, const void *report, size_t len)
{
    (void)dev;
    if (!report || len == 0) return -EINVAL;

    kprintf("[usb_hid] Parsing HID report descriptor (%zu bytes)...\n", len);

    /* Simple HID report descriptor parser */
    const uint8_t *data = (const uint8_t *)report;
    size_t pos = 0;

    /* HID report item types */
#define HID_ITEM_TAG_MAIN_INPUT   0x80
#define HID_ITEM_TAG_MAIN_OUTPUT  0x90
#define HID_ITEM_TAG_MAIN_FEATURE 0xB0
#define HID_ITEM_TAG_GLOBAL_USAGE_PAGE 0x04
#define HID_ITEM_TAG_GLOBAL_LOGICAL_MIN 0x14
#define HID_ITEM_TAG_GLOBAL_LOGICAL_MAX 0x24
#define HID_ITEM_TAG_GLOBAL_REPORT_SIZE 0x74
#define HID_ITEM_TAG_GLOBAL_REPORT_COUNT 0x94
#define HID_ITEM_TAG_LOCAL_USAGE  0x08

    uint32_t usage_page = 0;
    uint32_t usage = 0;
    uint32_t report_count = 0;
    int has_keyboard = 0;
    int has_mouse = 0;

    while (pos < len) {
        uint8_t item = data[pos++];
        if (item == 0) continue; /* padding */
        uint8_t tag = item & 0xFC;
        uint8_t type = item & 0x03;
        uint8_t size = item & 0xFC ? (item & 0x03) : 0;
        if (size == 3) size = 4; /* 3 means 4 bytes */

        if (pos + size > len) break;

        if (type == 1) { /* Global */
            switch (tag) {
            case HID_ITEM_TAG_GLOBAL_USAGE_PAGE:
                if (size == 1) usage_page = data[pos];
                else if (size == 2) usage_page = data[pos] | ((uint32_t)data[pos + 1] << 8);
                break;
            case HID_ITEM_TAG_GLOBAL_REPORT_SIZE:
                if (size == 1) /* report_size = */ (void)data[pos];
                else if (size == 2) /* report_size = */ (void)(data[pos] | ((uint32_t)data[pos + 1] << 8));
                break;
            case HID_ITEM_TAG_GLOBAL_REPORT_COUNT:
                if (size == 1) report_count = data[pos];
                else if (size == 2) report_count = data[pos] | ((uint32_t)data[pos + 1] << 8);
                break;
            default:
                break;
            }
        } else if (type == 2) { /* Local */
            if (tag == HID_ITEM_TAG_LOCAL_USAGE) {
                if (size == 1) usage = data[pos];
                else if (size == 2) usage = data[pos] | ((uint32_t)data[pos + 1] << 8);
            }
        } else if (type == 0) { /* Main */
            /* Check for Input/Output/Feature items */
            uint8_t main_tag = tag;
            (void)main_tag;
            if (usage_page == 0x01) { /* Generic Desktop Controls */
                if (usage == 0x06 && report_count > 0) {
                    has_keyboard = 1;
                }
                if (usage == 0x02) {
                    has_mouse = 1;
                }
            }
        }

        pos += size;
    }

    /* Update HID state based on parsed report */
    if (has_keyboard) {
        g_keyboard_present = 1;
        kprintf("[usb_hid] Report parsed: keyboard detected\n");
    }
    if (has_mouse) {
        g_mouse_present = 1;
        kprintf("[usb_hid] Report parsed: mouse detected\n");
    }

    if (!has_keyboard && !has_mouse) {
        kprintf("[usb_hid] Report parsed: unknown HID device (usage_page=0x%x, usage=0x%x)\n",
                usage_page, usage);
    }

    return 0;
}
