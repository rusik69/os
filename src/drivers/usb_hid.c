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
static int g_consumer_present = 0;
static int g_sysctrl_present = 0;
static int g_has_interrupt = 0;

/* Interface numbers for HID class requests */
static uint8_t g_kbd_intf = 0;
static uint8_t g_mouse_intf = 0;
static uint8_t g_consumer_intf = 0;
static uint8_t g_sysctrl_intf = 0;

/* Current keyboard LED state via USB SET_REPORT */
static uint8_t g_usb_led_state = 0;

/* Interrupt endpoint info */
static uint8_t g_int_in_ep = 0;
static uint8_t g_mouse_int_in_ep = 0;
static uint8_t g_consumer_int_in_ep = 0;
static uint8_t g_sysctrl_int_in_ep = 0;

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
static int g_mouse_wheel = 0;

/* Forward declarations */
static int usb_hid_parse_report_legacy(void *dev, const void *report, size_t len);

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

/* ── HID boot protocol control transfers ──────────────────────────── */

/*
 * GET_REPORT — request current report from the device.
 * USB HID Spec §7.2.1
 */
int usb_hid_get_report(uint8_t report_type, uint8_t report_id,
                        void *buf, size_t len)
{
    if (!g_hid_initialized || !buf || len == 0) return -EINVAL;

    uint16_t w_val = ((uint16_t)report_type << 8) | report_id;
    uint8_t iface = g_keyboard_present ? g_kbd_intf : g_mouse_intf;

    return usb_control(g_op_base, HID_REQTYPE_GET, HID_REQ_GET_REPORT,
                        w_val, iface, (uint16_t)len, buf, g_dev_addr);
}

/*
 * SET_REPORT — send output/feature report to the device.
 * USB HID Spec §7.2.4
 */
int usb_hid_set_report(uint8_t report_type, uint8_t report_id,
                        const void *buf, size_t len)
{
    if (!g_hid_initialized || !buf || len == 0) return -EINVAL;

    uint16_t w_val = ((uint16_t)report_type << 8) | report_id;
    uint8_t iface = g_keyboard_present ? g_kbd_intf : g_mouse_intf;

    return usb_control(g_op_base, HID_REQTYPE_SET, HID_REQ_SET_REPORT,
                        w_val, iface, (uint16_t)len, (void *)(uintptr_t)buf, g_dev_addr);
}

/*
 * GET_DESCRIPTOR (HID) — read the HID descriptor from the device.
 * The HID descriptor contains the HID spec version, country code,
 * and the length of the subordinate report descriptor.
 * USB HID Spec §6.2.1, §7.1.1
 */
int usb_hid_get_hid_descriptor(struct hid_descriptor *desc)
{
    if (!desc) return -EINVAL;

    return usb_control(g_op_base, 0x80, USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)(HID_DESC_HID << 8), 0,
                        sizeof(struct hid_descriptor), desc, g_dev_addr);
}

/*
 * Fetch the HID report descriptor and parse it for keyboard/mouse
 * detection using the report descriptor parser.
 */
int usb_hid_get_and_parse_report_descriptor(void)
{
    struct hid_descriptor hdesc;
    int rc = usb_hid_get_hid_descriptor(&hdesc);
    if (rc < 0) {
        kprintf("[USB HID] GET_DESCRIPTOR(HID) failed (%d)\n", rc);
        return rc;
    }

    uint16_t report_len = hdesc.wDescriptorLength0;
    if (report_len == 0 || report_len > 512) {
        kprintf("[USB HID] Invalid report descriptor length %u\n", report_len);
        return -EINVAL;
    }

    uint8_t *rdesc = (uint8_t *)hid_alloc_dma(report_len);
    if (!rdesc) return -ENOMEM;

    rc = usb_control(g_op_base, 0x80, USB_REQ_GET_DESCRIPTOR,
                      (uint16_t)(HID_DESC_REPORT << 8), 0,
                      report_len, rdesc, g_dev_addr);
    if (rc < 0) {
        kprintf("[USB HID] GET_DESCRIPTOR(REPORT) failed (%d)\n", rc);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)rdesc));
        return rc;
    }

    rc = usb_hid_parse_report_legacy(NULL, rdesc, report_len);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)rdesc));

    if (rc < 0) {
        kprintf("[USB HID] Report descriptor parse failed (%d)\n", rc);
        return rc;
    }

    kprintf("[USB HID] Report descriptor parsed (%u bytes)\n", report_len);
    return 0;
}

/*
 * Set keyboard LEDs via SET_REPORT (boot protocol output report).
 * The boot keyboard output report is a single byte:
 *   bit 0: Num Lock
 *   bit 1: Caps Lock
 *   bit 2: Scroll Lock
 *   bit 3: Compose
 *   bit 4: Kana
 */
int usb_hid_set_leds(uint8_t leds)
{
    if (!g_hid_initialized || !g_keyboard_present) return -ENODEV;

    int rc = usb_hid_set_report(HID_REPORT_OUTPUT, 0, &leds, 1);
    if (rc == 0) {
        g_usb_led_state = leds;
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

/*
 * Check if keycode was present in the previous report.
 * Returns 1 if found, 0 if not.
 */
static int key_was_down(uint8_t keycode)
{
    for (int i = 0; i < 6; i++) {
        if (g_last_kbd_report.keys[i] == keycode)
            return 1;
    }
    return 0;
}

/*
 * Process a boot protocol keyboard report with proper press/release
 * detection.  Only generates input for newly pressed keys (transition
 * from released to pressed).  Handles up to 6-key rollover.
 */
static void process_keyboard_report(const struct hid_keyboard_report *rep)
{
    /* Process newly pressed keys: keys in current report NOT in last report */
    for (int i = 0; i < 6; i++) {
        uint8_t kc = rep->keys[i];
        if (kc != HID_KEYCODE_NONE && kc != HID_KEYCODE_ERROR) {
            if (!key_was_down(kc)) {
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
    }

    memcpy(&g_last_kbd_report, rep, sizeof(g_last_kbd_report));
    g_kbd_changed = 1;
}

/* ── Mouse report processing ───────────────────────────────────────── */

static void process_mouse_report(const struct hid_mouse_report *rep) {
    g_mouse_buttons = rep->buttons;
    g_mouse_dx += rep->x_delta;
    g_mouse_dy += rep->y_delta;
    g_mouse_wheel += rep->wheel;
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

    /* Initialise consumer control subsystem */
    usb_hid_consumer_init();

    /* Initialise system control subsystem */
    usb_hid_sysctrl_init();

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

    /* Parse configuration descriptor for HID interfaces.
     * Use wTotalLength at offset 2-3, not bLength at offset 0 (which is always
     * 9 — just the config descriptor header — and misses all sub-descriptors
     * such as HID interfaces and endpoints). */
    uint16_t cfg_total_len = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
    if (cfg_total_len > 256) cfg_total_len = 256;
    int cfg_len = (int)cfg_total_len;
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
                    g_kbd_intf = if_num;
                    kprintf("[USB HID] Found keyboard (if=%d)\n", if_num);
                } else if (if_proto == 2) {  /* Mouse */
                    g_mouse_present = 1;
                    g_mouse_intf = if_num;
                    kprintf("[USB HID] Found mouse (if=%d)\n", if_num);
                } else {
                    /* Non-boot HID interface — could be consumer,
                     * multi-touch, joystick, system control, etc.  Save as
                     * potential endpoint and let the report descriptor
                     * parser determine the actual type. */
                    if (!g_consumer_present) {
                        g_consumer_present = 1;
                        g_consumer_intf = if_num;
                        kprintf("[USB HID] Found non-boot HID interface "
                                "(if=%d)\n", if_num);
                    }
                    /* Also flag as potential system control endpoint */
                    if (!g_sysctrl_present) {
                        g_sysctrl_present = 1;
                        g_sysctrl_intf = if_num;
                    }
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
                    } else if (g_consumer_present && !g_consumer_int_in_ep) {
                        g_consumer_int_in_ep = ep_addr & 0x0F;
                        kprintf("[USB HID] Consumer int IN ep=0x%02x\n", ep_addr);
                    } else if (g_sysctrl_present && !g_sysctrl_int_in_ep) {
                        g_sysctrl_int_in_ep = ep_addr & 0x0F;
                        kprintf("[USB HID] SysCtrl int IN ep=0x%02x\n", ep_addr);
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

    /* Fetch HID descriptor and parse report descriptor */
    usb_hid_get_and_parse_report_descriptor();

    /* Fetch initial keyboard report via GET_REPORT */
    if (g_keyboard_present) {
        uint8_t *init_kbd = (uint8_t *)hid_alloc_dma(8);
        if (init_kbd) {
            rc = usb_control(g_op_base, HID_REQTYPE_GET, HID_REQ_GET_REPORT,
                              (uint16_t)(HID_REPORT_INPUT << 8), g_kbd_intf,
                              8, init_kbd, g_dev_addr);
            if (rc == 0) {
                memcpy(&g_last_kbd_report, init_kbd, sizeof(g_last_kbd_report));
            }
            pmm_free_frame(VIRT_TO_PHYS((uint64_t)init_kbd));
        }
    }

    /* Fetch initial mouse report via GET_REPORT */
    if (g_mouse_present) {
        uint8_t *init_mouse = (uint8_t *)hid_alloc_dma(4);
        if (init_mouse) {
            rc = usb_control(g_op_base, HID_REQTYPE_GET, HID_REQ_GET_REPORT,
                              (uint16_t)(HID_REPORT_INPUT << 8), g_mouse_intf,
                              4, init_mouse, g_dev_addr);
            if (rc == 0) {
                process_mouse_report((const struct hid_mouse_report *)init_mouse);
            }
            pmm_free_frame(VIRT_TO_PHYS((uint64_t)init_mouse));
        }
    }

    g_hid_initialized = 1;
    kprintf("[USB HID] Initialized: %s%s%s%s\n",
            g_keyboard_present ? "keyboard " : "",
            g_mouse_present ? "mouse " : "",
            g_consumer_present ? "consumer " : "",
            g_sysctrl_present ? "sysctrl " : "");
    return 0;
}

/* ── Poll HID devices ──────────────────────────────────────────────── */

void usb_hid_poll(void) {
    if (!g_hid_initialized || !g_has_interrupt) return;
    hid_poll_interrupt_in();
    /* Also poll consumer endpoint if present and separate from keyboard */
    if (g_consumer_present && g_consumer_int_in_ep &&
        g_consumer_int_in_ep != g_int_in_ep) {
        usb_hid_consumer_poll();
    }
    /* Also poll system control endpoint if present and separate */
    if (g_sysctrl_present && g_sysctrl_int_in_ep &&
        g_sysctrl_int_in_ep != g_int_in_ep &&
        g_sysctrl_int_in_ep != g_consumer_int_in_ep) {
        usb_hid_sysctrl_poll();
    }
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

int usb_hid_mouse_wheel_get(void)
{
    int w = g_mouse_wheel;
    g_mouse_wheel = 0;
    return w;
}

static int usb_hid_read(struct usb_device *dev, void *buf, size_t count)
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
            /* Wheel at offset 3 */
            p[3] = (uint8_t)usb_hid_mouse_wheel_get();
            /* Clear remainder */
            memset(p + 4, 0, count - 4);
            return 4;
        }
    }

    return 0;
}

/* ── Helper: read a value of a given byte size from the stream ──── */
static uint32_t hid_read_value(const uint8_t *data, int bsize)
{
    switch (bsize) {
    case 0:  return 0;
    case 1:  return data[0];
    case 2:  return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
    case 4:  return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
                       | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    default: return 0;
    }
}

/* ── Helper: read a signed value of a given byte size ───────────── */
static int32_t hid_read_svalue(const uint8_t *data, int bsize)
{
    uint32_t v = hid_read_value(data, bsize);
    /* Sign-extend based on bsize */
    switch (bsize) {
    case 1:  return (int32_t)(int8_t)(v & 0xFF);
    case 2:  return (int32_t)(int16_t)(v & 0xFFFF);
    case 4:  return (int32_t)v;
    default: return 0;
    }
}

/* ── Reset local state to defaults ──────────────────────────────── */
static void hid_reset_local(struct hid_local_state *ls)
{
    ls->usage              = 0;
    ls->usage_minimum      = 0;
    ls->usage_maximum      = 0;
    ls->designator_index   = 0;
    ls->designator_minimum = 0;
    ls->designator_maximum = 0;
    ls->string_index       = 0;
    ls->string_minimum     = 0;
    ls->string_maximum     = 0;
    ls->delimiter          = 0;
}

/* ── Decode the collection type name ────────────────────────────── */
static const char *hid_collection_name(uint8_t type)
{
    switch (type) {
    case HID_COLLECTION_PHYSICAL:     return "Physical";
    case HID_COLLECTION_APPLICATION:  return "Application";
    case HID_COLLECTION_LOGICAL:      return "Logical";
    case HID_COLLECTION_REPORT:       return "Report";
    case HID_COLLECTION_NAMED_ARRAY:  return "Named Array";
    case HID_COLLECTION_USAGE_SWITCH: return "Usage Switch";
    case HID_COLLECTION_USAGE_MODIFIER: return "Usage Modifier";
    default:                          return "Unknown";
    }
}

/* ── Parse a raw HID report descriptor ──────────────────────────── */
int usb_hid_parse_report(void *dev, const void *report, size_t len,
                         struct hid_report_desc *out)
{
    (void)dev;
    if (!report || len == 0 || !out) return -EINVAL;

    const uint8_t *data = (const uint8_t *)report;
    size_t pos = 0;

    /* Initialise output structure */
    memset(out, 0, sizeof(*out));

    /* Initialise global state */
    struct hid_global_state gs;
    memset(&gs, 0, sizeof(gs));

    /* Initialise local state */
    struct hid_local_state ls;
    hid_reset_local(&ls);

    kprintf("[usb_hid] Parsing HID report descriptor (%llu bytes)...\n", (unsigned long long)len);

    while (pos < len && out->num_items < HID_REPORT_MAX_ITEMS) {
        uint8_t prefix = data[pos];

        /* Skip padding bytes */
        if (prefix == 0) {
            pos++;
            continue;
        }

        /* ── Long item ─────────────────────────────────────────── */
        if (prefix == HID_LONG_ITEM_PREFIX) {
            /* Long item: FE <bSize> <bTag> <data...> */
            if (pos + 2 > len) break;
            uint8_t long_size = data[pos + 1];
            uint8_t long_tag  = data[pos + 2];
            size_t long_total = (size_t)3 + long_size;
            if (pos + long_total > len) break;

            kprintf("[usb_hid]   Long item: tag=0x%02x, size=%u\n", long_tag, long_size);
            pos += long_total;
            continue;
        }

        /* ── Short item ─────────────────────────────────────────── */
        uint8_t bTag  = (prefix >> 4) & 0x0F;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bSize = prefix & 0x03;

        /* Decode bSize: 0→0, 1→1, 2→2, 3→4 */
        int data_bytes = (bSize == 3) ? 4 : (int)bSize;

        if (pos + 1 + data_bytes > len) break;
        pos++;  /* consume prefix byte */

        const uint8_t *item_data = data + pos;

        /* Decode value based on signed/unsigned convention per item type */
        uint32_t uval = hid_read_value(item_data, data_bytes);
        int32_t  sval = hid_read_svalue(item_data, data_bytes);

        switch (bType) {
        /* ── Main items (bType=0) ──────────────────────────────── */
        case HID_TYPE_MAIN: {
            switch (bTag) {
            case 8: { /* Input */
                struct hid_report_item *ri = &out->items[out->num_items++];
                ri->tag   = HID_ITEM_INPUT;
                ri->flags = uval;
                ri->data  = uval;
                ri->global = gs;
                ri->local  = ls;
                ri->collection_depth = out->collection_stack_depth;
                hid_reset_local(&ls);
                break;
            }
            case 9: { /* Output */
                struct hid_report_item *ri = &out->items[out->num_items++];
                ri->tag   = HID_ITEM_OUTPUT;
                ri->flags = uval;
                ri->data  = uval;
                ri->global = gs;
                ri->local  = ls;
                ri->collection_depth = out->collection_stack_depth;
                hid_reset_local(&ls);
                break;
            }
            case 10: { /* Collection */
                if (out->num_collections < HID_REPORT_MAX_COLLECTIONS) {
                    struct hid_collection *c = &out->collections[out->num_collections++];
                    c->type       = (uint8_t)(uval & 0xFF);
                    c->usage_page = gs.usage_page;
                    c->usage      = ls.usage;

                    /* Track nesting */
                    if (out->collection_stack_depth < HID_REPORT_MAX_COLLECTIONS) {
                        out->collection_stack[out->collection_stack_depth++] =
                            out->num_collections - 1;
                    }

                    kprintf("[usb_hid]   Collection: type=%s (0x%02x), "
                            "usage_page=0x%x, usage=0x%x\n",
                            hid_collection_name(c->type), c->type,
                            c->usage_page, c->usage);
                }

                /* Also record as a parsed item */
                if (out->num_items < HID_REPORT_MAX_ITEMS) {
                    struct hid_report_item *ri = &out->items[out->num_items++];
                    ri->tag   = HID_ITEM_COLLECTION;
                    ri->flags = uval;
                    ri->data  = uval;
                    ri->global = gs;
                    ri->local  = ls;
                    ri->collection_depth = out->collection_stack_depth - 1;
                }
                hid_reset_local(&ls);
                break;
            }
            case 11: { /* Feature */
                struct hid_report_item *ri = &out->items[out->num_items++];
                ri->tag   = HID_ITEM_FEATURE;
                ri->flags = uval;
                ri->data  = uval;
                ri->global = gs;
                ri->local  = ls;
                ri->collection_depth = out->collection_stack_depth;
                hid_reset_local(&ls);
                break;
            }
            case 12: { /* End Collection */
                /* Pop collection stack */
                if (out->collection_stack_depth > 0) {
                    out->collection_stack_depth--;
                }

                /* Also record as a parsed item */
                if (out->num_items < HID_REPORT_MAX_ITEMS) {
                    struct hid_report_item *ri = &out->items[out->num_items++];
                    ri->tag   = HID_ITEM_END_COLLECTION;
                    ri->flags = 0;
                    ri->data  = 0;
                    ri->global = gs;
                    ri->local  = ls;
                    ri->collection_depth = out->collection_stack_depth;
                }
                break;
            }
            default:
                kprintf("[usb_hid]   Unknown Main item: bTag=%u, data=0x%x\n",
                        bTag, uval);
                break;
            }
            break;
        }

        /* ── Global items (bType=1) ────────────────────────────── */
        case HID_TYPE_GLOBAL: {
            switch (bTag) {
            case 0:  /* Usage Page */
                gs.usage_page = uval;
                break;
            case 1:  /* Logical Minimum */
                gs.logical_minimum = sval;
                break;
            case 2:  /* Logical Maximum */
                gs.logical_maximum = sval;
                break;
            case 3:  /* Physical Minimum */
                gs.physical_minimum = sval;
                break;
            case 4:  /* Physical Maximum */
                gs.physical_maximum = sval;
                break;
            case 5:  /* Unit Exponent */
                gs.unit_exponent = uval;
                break;
            case 6:  /* Unit */
                gs.unit = uval;
                break;
            case 7:  /* Report Size */
                gs.report_size = uval;
                break;
            case 8:  /* Report ID */
                gs.report_id = uval;
                kprintf("[usb_hid]   Report ID: %u\n", uval);
                break;
            case 9:  /* Report Count */
                gs.report_count = uval;
                break;
            case 10: /* Push — save global state onto stack */
                if (out->global_stack_depth < HID_GLOBAL_STACK_DEPTH) {
                    out->global_stack[out->global_stack_depth++] = gs;
                }
                break;
            case 11: /* Pop — restore global state from stack */
                if (out->global_stack_depth > 0) {
                    gs = out->global_stack[--out->global_stack_depth];
                }
                break;
            default:
                break;
            }
            break;
        }

        /* ── Local items (bType=2) ─────────────────────────────── */
        case HID_TYPE_LOCAL: {
            switch (bTag) {
            case 0:  /* Usage */
                ls.usage = uval;
                break;
            case 1:  /* Usage Minimum */
                ls.usage_minimum = uval;
                break;
            case 2:  /* Usage Maximum */
                ls.usage_maximum = uval;
                break;
            case 3:  /* Designator Index */
                ls.designator_index = uval;
                break;
            case 4:  /* Designator Minimum */
                ls.designator_minimum = uval;
                break;
            case 5:  /* Designator Maximum */
                ls.designator_maximum = uval;
                break;
            case 6:  /* String Index */
                ls.string_index = uval;
                break;
            case 7:  /* String Minimum */
                ls.string_minimum = uval;
                break;
            case 8:  /* String Maximum */
                ls.string_maximum = uval;
                break;
            case 9:  /* Delimiter */
                ls.delimiter = uval;
                break;
            default:
                break;
            }
            break;
        }

        /* ── Reserved (bType=3) ────────────────────────────────── */
        default:
            break;
        }

        pos += (size_t)data_bytes;
    }

    kprintf("[usb_hid] Parsed %d report items, %d collections\n",
            out->num_items, out->num_collections);

    return 0;
}

/* ── Legacy parser (single-call, no structured output) ──────────── */
int usb_hid_parse_report_legacy(void *dev, const void *report, size_t len)
{
    struct hid_report_desc desc;
    int rc = usb_hid_parse_report(dev, report, len, &desc);
    if (rc < 0) return rc;

    /* Scan parsed items for keyboard/mouse/consumer/system control detection */
    int has_keyboard = 0;
    int has_mouse = 0;
    int has_consumer = 0;
    int has_sysctrl = 0;

    for (int i = 0; i < desc.num_items; i++) {
        struct hid_report_item *ri = &desc.items[i];
        if (ri->tag != HID_ITEM_INPUT && ri->tag != HID_ITEM_OUTPUT &&
            ri->tag != HID_ITEM_FEATURE)
            continue;

        uint32_t up = ri->global.usage_page;
        uint32_t us = ri->local.usage;

        /* Generic Desktop: Keyboard (usage=0x06) */
        if (up == HID_PAGE_GENERIC_DESKTOP && us == HID_USAGE_KEYBOARD &&
            ri->global.report_count > 0) {
            has_keyboard = 1;
        }

        /* Generic Desktop: Mouse (usage=0x02) */
        if (up == HID_PAGE_GENERIC_DESKTOP && us == HID_USAGE_MOUSE) {
            has_mouse = 1;
        }

        /* Consumer Page: any usage in Consumer Control */
        if (up == HID_PAGE_CONSUMER) {
            has_consumer = 1;
        }

        /* Generic Desktop: System Control (usage=0x80) */
        if (up == HID_PAGE_GENERIC_DESKTOP && us == HID_USAGE_SYSTEM_CONTROL) {
            has_sysctrl = 1;
        }
    }

    /* Also check collection-level usages (more reliable for consumer) */
    for (int j = 0; j < desc.num_collections; j++) {
        if (desc.collections[j].usage_page == HID_PAGE_GENERIC_DESKTOP) {
            if (desc.collections[j].usage == HID_USAGE_KEYBOARD)
                has_keyboard = 1;
            if (desc.collections[j].usage == HID_USAGE_MOUSE)
                has_mouse = 1;
        }
        /* Consumer Control application collection */
        if (desc.collections[j].usage_page == HID_PAGE_CONSUMER &&
            desc.collections[j].usage == HID_USAGE_CONSUMER_CONTROL) {
            has_consumer = 1;
        }
        /* System Control application collection */
        if (desc.collections[j].usage_page == HID_PAGE_GENERIC_DESKTOP &&
            desc.collections[j].usage == HID_USAGE_SYSTEM_CONTROL) {
            has_sysctrl = 1;
        }
    }

    if (has_keyboard) {
        g_keyboard_present = 1;
        kprintf("[usb_hid] Report parsed: keyboard detected\n");
    }
    if (has_mouse) {
        g_mouse_present = 1;
        kprintf("[usb_hid] Report parsed: mouse detected\n");
    }
    if (has_consumer) {
        g_consumer_present = 1;
        kprintf("[usb_hid] Report parsed: consumer control detected\n");
    }
    if (has_sysctrl) {
        kprintf("[usb_hid] Report parsed: system control detected\n");
    }
    if (!has_keyboard && !has_mouse && !has_consumer && !has_sysctrl) {
        kprintf("[usb_hid] Report parsed: unknown HID device\n");
    }

    /* If consumer detected, register with the consumer driver */
    if (has_consumer) {
        usb_hid_consumer_register(g_dev_addr, g_consumer_intf,
                                   g_consumer_int_in_ep,
                                   (const uint8_t *)report, (int)len);
    }

    /* If system control detected, register with the sysctrl driver */
    if (has_sysctrl) {
        usb_hid_sysctrl_register(g_dev_addr, g_sysctrl_intf,
                                  g_sysctrl_int_in_ep,
                                  (const uint8_t *)report, (int)len);
    }

    return 0;
}
