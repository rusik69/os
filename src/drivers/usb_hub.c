/*
 * usb_hub.c — USB hub support for EHCI
 *
 * Provides hub descriptor parsing, port status change monitoring,
 * and multi-TT support for high-speed hubs.  Port events are
 * propagated to the USB core for device discovery.
 *
 * References:
 *   USB 2.0 Specification, Chapter 11 — Hub
 *   USB 2.0 Hub Specification, Revision 1.0
 */

#include "usb.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "delay.h"

/* ── Hub class constants ───────────────────────────────────────────── */

#define USB_CLASS_HUB 0x09

/* Hub descriptor type */
#define HUB_DESC_TYPE 0x29

/* Hub features */
#define HUB_FEATURE_PORT_RESET         4
#define HUB_FEATURE_PORT_POWER         8
#define HUB_FEATURE_PORT_ENABLE        1
#define HUB_FEATURE_C_PORT_CONNECTION  16
#define HUB_FEATURE_C_PORT_ENABLE      17
#define HUB_FEATURE_C_PORT_SUSPEND     18
#define HUB_FEATURE_C_PORT_OVER_CURRENT 19
#define HUB_FEATURE_C_PORT_RESET       20

/* Port status bits */
#define PORT_STATUS_CONNECTION    (1U << 0)
#define PORT_STATUS_ENABLE        (1U << 1)
#define PORT_STATUS_SUSPEND       (1U << 2)
#define PORT_STATUS_OVER_CURRENT  (1U << 3)
#define PORT_STATUS_RESET         (1U << 4)
#define PORT_STATUS_POWER         (1U << 8)
#define PORT_STATUS_LOW_SPEED     (1U << 9)
#define PORT_STATUS_HIGH_SPEED    (1U << 10)

/* Port change bits */
#define PORT_CHANGE_C_CONNECTION    (1U << 0)
#define PORT_CHANGE_C_ENABLE        (1U << 1)
#define PORT_CHANGE_C_SUSPEND       (1U << 2)
#define PORT_CHANGE_C_OVER_CURRENT  (1U << 3)
#define PORT_CHANGE_C_RESET         (1U << 4)

/* Hub descriptor (as returned by GET_DESCRIPTOR) */
struct hub_descriptor {
    uint8_t  bDescLength;       /* descriptor length */
    uint8_t  bDescriptorType;   /* 0x29 */
    uint8_t  bNbrPorts;         /* number of downstream ports */
    uint16_t wHubCharacteristics; /* hub characteristics */
    uint8_t  bPwrOn2PwrGood;    /* power-on to power-good time (2 ms units) */
    uint8_t  bHubContrCurrent;  /* max current consumption */
    /* Variable-length: DeviceRemovable[], PortPwrCtrlMask[] follow */
} __attribute__((packed));

/* Hub characteristics flags */
#define HUB_CHAR_TT_GLOBAL      (0 << 5)  /* single Transaction Translator */
#define HUB_CHAR_TT_PER_PORT    (1U << 5)  /* per-port Transaction Translator */

/* ── EHCI transfer primitives (same as other USB drivers) ──────────── */

#define MAX_PKT 64

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

static uint64_t g_hub_op_base = 0;
static int      g_hub_initialized = 0;

/* Hub info */
#define USB_MAX_HUBS 4
#define USB_MAX_PORTS_PER_HUB 8

struct hub_state {
    uint8_t  dev_addr;
    uint8_t  n_ports;
    uint16_t characteristics;
    uint8_t  power_good_delay;  /* in 2 ms units */
    uint8_t  port_powered[USB_MAX_PORTS_PER_HUB];
    uint16_t port_status[USB_MAX_PORTS_PER_HUB];
    uint64_t debounce_until[USB_MAX_PORTS_PER_HUB]; /* timestamp when debounce completes */
    uint8_t  debounce_pending[USB_MAX_PORTS_PER_HUB]; /* non-zero if debounce in progress */
};

static struct hub_state g_hubs[USB_MAX_HUBS];
static int g_hub_count = 0;

/* ── Forward declarations for static functions ───────────────────── */
static int hub_port_debounce(struct hub_state *hub, int port, uint16_t status);
static int hub_port_over_current_recover(struct hub_state *hub, int port);

/* ── DMA / MMIO helpers ────────────────────────────────────────────── */

static void *hub_alloc_dma(size_t sz) {
    (void)sz;
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return (void *)0;
    void *p = PHYS_TO_VIRT(frame);
    memset(p, 0, 4096);
    return p;
}

static inline uint32_t op_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_hub_op_base + off);
}
static inline void op_wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_hub_op_base + off) = val;
}
static void busy_wait(volatile int n) { while (n-- > 0) __asm__("pause"); }

/* ── EHCI transfer ─────────────────────────────────────────────────── */

static int ehci_do_transfer(uint8_t dev_addr, uint32_t pid, uint8_t ep,
                             void *data, uint32_t len, int toggle) {
    if (!g_hub_op_base) return -1;

    struct ehci_qh  *qh  = (struct ehci_qh  *)hub_alloc_dma(sizeof(*qh));
    struct ehci_qtd *qtd = (struct ehci_qtd *)hub_alloc_dma(sizeof(*qtd));
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
    qh->ep_char  = QH_DEVADDR(dev_addr) | QH_EP(ep) | QH_EPS_HS
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
static int usb_control(uint8_t dev_addr, uint8_t bm_req_type, uint8_t b_req,
                        uint16_t w_val, uint16_t w_idx, uint16_t w_len,
                        void *data) {
    uint8_t *setup = (uint8_t *)hub_alloc_dma(8);
    if (!setup) return -1;
    setup[0] = bm_req_type;
    setup[1] = b_req;
    setup[2] = (uint8_t)(w_val & 0xFF);
    setup[3] = (uint8_t)(w_val >> 8);
    setup[4] = (uint8_t)(w_idx & 0xFF);
    setup[5] = (uint8_t)(w_idx >> 8);
    setup[6] = (uint8_t)(w_len & 0xFF);
    setup[7] = (uint8_t)(w_len >> 8);

    int rc = ehci_do_transfer(dev_addr, QTD_PID_SETUP, 0, setup, 8, 0);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)setup));
    if (rc < 0) return rc;

    if (w_len && data) {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_IN : QTD_PID_OUT);
        rc = ehci_do_transfer(dev_addr, pid, 0, data, w_len, 1);
        if (rc < 0) return rc;
    }

    {
        uint32_t pid = (uint32_t)((bm_req_type & 0x80) ? QTD_PID_OUT : QTD_PID_IN);
        uint8_t *dummy = (uint8_t *)hub_alloc_dma(4);
        if (!dummy) return -1;
        rc = ehci_do_transfer(dev_addr, pid, 0, dummy, 0, 1);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)dummy));
    }
    return rc;
}

/* ── Hub control requests ──────────────────────────────────────────── */

/* Clear a hub feature */
static int hub_clear_feature(uint8_t dev_addr, uint16_t feature, uint16_t port) {
    return usb_control(dev_addr, 0x23, 0x01, feature, port, 0, NULL);
}

/* Set a port feature */
static int hub_set_port_feature(uint8_t dev_addr, uint16_t feature, uint16_t port) {
    return usb_control(dev_addr, 0x23, 0x03, feature, port, 0, NULL);
}

/* Get port status */
static int hub_get_port_status(uint8_t dev_addr, uint8_t port,
                                uint16_t *status, uint16_t *change) {
    uint8_t *buf = (uint8_t *)hub_alloc_dma(4);
    if (!buf) return -1;

    int rc = usb_control(dev_addr, 0xA3, 0x00, 0, port, 4, buf);
    if (rc == 0) {
        if (status) *status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if (change) *change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    }

    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return rc;
}

/* ── Enumeration of devices behind a hub ───────────────────────────── */

static void hub_enum_device(struct hub_state *hub, uint8_t port) {
    kprintf("[USB HUB] Enumerating device on hub addr=%d port %d\n",
            hub->dev_addr, port);

    /* Reset the port */
    hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_RESET, port);
    busy_wait(500000);  /* 50 ms reset */
    hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_RESET, port);

    /* Wait for power-good */
    busy_wait((uint32_t)hub->power_good_delay * 2000);  /* 2 ms units */

    /* Read port status to determine speed */
    uint16_t status = 0, change = 0;
    hub_get_port_status(hub->dev_addr, port, &status, &change);

    if (!(status & PORT_STATUS_CONNECTION)) return;
    if (!(status & PORT_STATUS_ENABLE)) return;

    int speed = 0;  /* full speed (12 Mbps) */
    if (status & PORT_STATUS_HIGH_SPEED)
        speed = 2;  /* high speed (480 Mbps) */
    else if (status & PORT_STATUS_LOW_SPEED)
        speed = 1;  /* low speed (1.5 Mbps) */

    /* Register with core USB layer */
    if (usb_get_device_count() < USB_MAX_DEVICES) {
        struct usb_device *dev = usb_get_device(usb_get_device_count());
        if (dev) {
            memset(dev, 0, sizeof(*dev));
            dev->addr  = (uint8_t)(usb_get_device_count() + 2);
            dev->speed = (uint8_t)speed;
            /* We can't read class here without more control transfers */
            dev->class_code = 0xFF;
        }
    }

    kprintf("[USB HUB] Device on addr=%d port %d speed=%s\n",
            hub->dev_addr, port, speed == 2 ? "high" : "full");
}

/* ── Hub initialisation ────────────────────────────────────────────── */

static int enumerate_hub(uint8_t dev_addr) {
    if (g_hub_count >= USB_MAX_HUBS) return -1;

    /* GET_DESCRIPTOR for hub descriptor */
    uint8_t *buf = (uint8_t *)hub_alloc_dma(64);
    if (!buf) return -1;

    int rc = usb_control(dev_addr, 0xA0, 0x06, 0x2900, 0, 64, buf);
    if (rc < 0) {
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
        return rc;
    }

    struct hub_descriptor *desc = (struct hub_descriptor *)buf;

    /* Validate hub descriptor */
    if (desc->bDescriptorType != HUB_DESC_TYPE) {
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
        return -2;
    }

    struct hub_state *hub = &g_hubs[g_hub_count];
    hub->dev_addr = dev_addr;
    hub->n_ports = desc->bNbrPorts;
    hub->characteristics = desc->wHubCharacteristics;
    hub->power_good_delay = desc->bPwrOn2PwrGood;

    kprintf("[USB HUB] Hub addr=%d: %d ports, chars=0x%04x, pwrgood=%d\n",
            dev_addr, hub->n_ports, hub->characteristics, hub->power_good_delay);

    /* Check for multi-TT support */
    if (hub->characteristics & HUB_CHAR_TT_PER_PORT) {
        kprintf("[USB HUB] Multi-TT hub\n");
    } else {
        kprintf("[USB HUB] Single-TT hub\n");
    }

    /* Power up all ports */
    for (int p = 1; p <= hub->n_ports && p <= USB_MAX_PORTS_PER_HUB; p++) {
        hub_set_port_feature(dev_addr, HUB_FEATURE_PORT_POWER, p);
        hub->port_powered[p - 1] = 1;
    }

    /* Wait for power to stabilize */
    busy_wait((uint32_t)hub->power_good_delay * 2000);

    /* Enumerate already-connected devices */
    for (int p = 1; p <= hub->n_ports; p++) {
        uint16_t status = 0, change = 0;
        hub_get_port_status(dev_addr, p, &status, &change);

        if (status & PORT_STATUS_CONNECTION) {
            hub_enum_device(hub, p);
        }
    }

    g_hub_count++;
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return 0;
}

/* ── Periodic hub polling ──────────────────────────────────────────── */

void usb_hub_poll(void) {
    if (!g_hub_initialized) return;

    for (int h = 0; h < g_hub_count; h++) {
        struct hub_state *hub = &g_hubs[h];

        for (int p = 1; p <= hub->n_ports; p++) {
            uint16_t status = 0, change = 0;
            if (hub_get_port_status(hub->dev_addr, (uint8_t)p, &status, &change) < 0)
                continue;

            /* Connection change detected */
            if (change & PORT_CHANGE_C_CONNECTION) {
                /* Clear the change bit first */
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_CONNECTION, (uint16_t)p);

                if (status & PORT_STATUS_CONNECTION) {
                    kprintf("[USB HUB] Port %d: device connected, debouncing...\n", p);
                    /* Start debounce */
                    int db = hub_port_debounce(hub, p, status);
                    if (db < 0) {
                        kprintf("[USB HUB] Port %d: debounce error\n", p);
                    }
                } else {
                    kprintf("[USB HUB] Port %d: device disconnected\n", p);
                    /* Clear any pending debounce */
                    hub->debounce_pending[p - 1] = 0;
                }
            }

            /* If we had a pending debounce, check it */
            if (hub->debounce_pending[p - 1]) {
                int db = hub_port_debounce(hub, p, status);
                if (db > 0) {
                    /* Debounce passed — connection is stable */
                    kprintf("[USB HUB] Port %d: debounce stable, enumerating...\n", p);
                    hub_enum_device(hub, (uint8_t)p);
                } else if (db < 0) {
                    kprintf("[USB HUB] Port %d: debounce failed\n", p);
                    hub->debounce_pending[p - 1] = 0;
                }
                /* db == 0: still debouncing, continue polling */
            }

            /* Over-current change */
            if (change & PORT_CHANGE_C_OVER_CURRENT) {
                kprintf("[USB HUB] Port %d: over-current detected\n", p);
                hub_port_over_current_recover(hub, p);
            }

            /* Enable change */
            if (change & PORT_CHANGE_C_ENABLE) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_ENABLE, (uint16_t)p);
                kprintf("[USB HUB] Port %d: %s\n", p,
                        (status & PORT_STATUS_ENABLE) ? "enabled" : "disabled");
            }

            /* Suspend change */
            if (change & PORT_CHANGE_C_SUSPEND) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_SUSPEND, (uint16_t)p);
                kprintf("[USB HUB] Port %d: %s\n", p,
                        (status & PORT_STATUS_SUSPEND) ? "suspended" : "resumed");
            }

            /* Reset complete */
            if (change & PORT_CHANGE_C_RESET) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_RESET, (uint16_t)p);
                kprintf("[USB HUB] Port %d: reset completed\n", p);
            }

            /* Update cached status */
            hub->port_status[p - 1] = status;
        }
    }
}

/* ── Initialisation ────────────────────────────────────────────────── */

int usb_hub_init(void) {
    if (g_hub_initialized) return 0;

    g_hub_op_base = ehci_get_op_base();
    if (!g_hub_op_base) return -1;

    /* Check for hub-class devices already detected by EHCI */
    int n_devices = usb_get_device_count();
    int hub_found = 0;

    for (int i = 0; i < n_devices; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) continue;

        /* For now, check class code once we find hub devices */
        /* Hub detection address: first hub gets address after root devices */
        uint8_t hub_addr = (uint8_t)(i + 2);

        /* Read device descriptor to check class */
        uint8_t *desc = (uint8_t *)hub_alloc_dma(18);
        if (!desc) continue;

        int rc = usb_control(hub_addr, 0x80, 0x06, 0x0100, 0, 18, desc);
        if (rc == 0 && desc[4] == USB_CLASS_HUB) {
            kprintf("[USB HUB] Found hub device on address %d\n", hub_addr);

            /* SET_ADDRESS already done by port reset; set configuration */
            rc = usb_control(hub_addr, 0x00, 0x09, 1, 0, 0, NULL);
            if (rc == 0) {
                enumerate_hub(hub_addr);
                hub_found = 1;
            }
        }

        pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc));
    }

    g_hub_initialized = 1;

    if (!hub_found) {
        kprintf("[USB HUB] No hub devices found (passive mode)\n");
    }

    return 0;
}

/* ── Hub port reset ────────────────────────────────────────── */
int usb_hub_port_reset(void *hub_ptr, int port)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    /* Set PORT_RESET feature on the hub */
    int rc = hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_RESET, (uint16_t)port);
    if (rc < 0)
        return rc;

    /* Wait for reset to complete (USB 2.0 spec: 50 ms reset time) */
    mdelay(50);

    /* Clear C_PORT_RESET change bit */
    hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_RESET, (uint16_t)port);

    return 0;
}

/* ── Hub port enable/disable ───────────────────────────────── */
int usb_hub_port_enable(void *hub_ptr, int port)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    /* Set PORT_ENABLE feature to enable the port */
    return hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
}

/* ── Hub port disable ───────────────────────────────────────── */
int usb_hub_port_disable(void *hub_ptr, int port)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    /* Clear PORT_ENABLE to disable the port */
    return hub_clear_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
}

/* ── Hub port power control ─────────────────────────────────── */
int usb_hub_port_power(void *hub_ptr, int port, int on)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    if (on) {
        return hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_POWER, (uint16_t)port);
    } else {
        return hub_clear_feature(hub->dev_addr, HUB_FEATURE_PORT_POWER, (uint16_t)port);
    }
}

/* ── Hub port over-current recovery ─────────────────────────── */
static int hub_port_over_current_recover(struct hub_state *hub, int port)
{
    kprintf("[USB HUB] Port %d: over-current condition, disabling port\n", port);

    /* Disable the port */
    int rc = hub_clear_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
    if (rc < 0)
        return rc;

    /* Clear the over-current change */
    rc = hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_OVER_CURRENT, (uint16_t)port);
    if (rc < 0)
        return rc;

    /* Wait 100 ms for over-current to clear */
    mdelay(100);

    /* Re-enable the port */
    rc = hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
    if (rc < 0)
        return rc;

    kprintf("[USB HUB] Port %d: over-current cleared, port re-enabled\n", port);
    return 0;
}

/* ── Hub port debounce ──────────────────────────────────────── */
/*
 * Debounce a port connection change.  Returns 1 when the connection
 * is stable (debounce passed), 0 while still debouncing, negative on error.
 */
static int hub_port_debounce(struct hub_state *hub, int port, uint16_t status)
{
    uint64_t now = timer_get_ticks();

    /* Debounce time: 100 ms (USB 2.0 spec §7.1.7.3: 100 ms debounce) */
    const uint64_t debounce_ms = 100;

    if (!hub->debounce_pending[port - 1]) {
        /* Start debouncing */
        hub->debounce_pending[port - 1] = 1;
        hub->debounce_until[port - 1] = now + (debounce_ms * 100 / 1000);
        return 0;
    }

    /* Check if debounce timer expired */
    if (now < hub->debounce_until[port - 1])
        return 0;

    /* Debounce done — check if the connection state is still valid */
    hub->debounce_pending[port - 1] = 0;

    uint16_t final_status = 0;
    uint16_t final_change = 0;
    if (hub_get_port_status(hub->dev_addr, (uint8_t)port, &final_status, &final_change) < 0)
        return -1;

    if (!!(final_status & PORT_STATUS_CONNECTION) != !!(status & PORT_STATUS_CONNECTION)) {
        /* State changed during debounce — restart */
        kprintf("[USB HUB] Port %d: connection state changed during debounce, "
                "restarting\n", port);
        return 0;  /* caller will re-detect on next poll */
    }

    return 1;  /* Connection stable */
}

/* ── Detect port status changes on all hubs ─────────────────── */
int usb_hub_detect(void)
{
    int changes = 0;

    for (int h = 0; h < g_hub_count; h++) {
        struct hub_state *hub = &g_hubs[h];

        for (int p = 1; p <= hub->n_ports; p++) {
            uint16_t status = 0, change = 0;
            if (hub_get_port_status(hub->dev_addr, (uint8_t)p, &status, &change) < 0)
                continue;

            if (change != 0) {
                changes++;
                kprintf("[USB HUB] Hub %d port %d: status=0x%04x change=0x%04x\n",
                        h, p, status, change);
            }

            /* Handle connection changes with debounce */
            if (change & PORT_CHANGE_C_CONNECTION) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_CONNECTION, (uint16_t)p);
                kprintf("[USB HUB] Port %d: connection change%s\n", p,
                        (status & PORT_STATUS_CONNECTION) ? " (connected)" : " (disconnected)");
            }

            /* Handle over-current */
            if (change & PORT_CHANGE_C_OVER_CURRENT) {
                hub_port_over_current_recover(hub, p);
            }

            /* Handle enable changes */
            if (change & PORT_CHANGE_C_ENABLE) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_ENABLE, (uint16_t)p);
                kprintf("[USB HUB] Port %d: enable change -> %s\n", p,
                        (status & PORT_STATUS_ENABLE) ? "enabled" : "disabled");
            }

            /* Handle suspend changes */
            if (change & PORT_CHANGE_C_SUSPEND) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_SUSPEND, (uint16_t)p);
                kprintf("[USB HUB] Port %d: suspend change -> %s\n", p,
                        (status & PORT_STATUS_SUSPEND) ? "suspended" : "resumed");
            }

            /* Handle reset completion */
            if (change & PORT_CHANGE_C_RESET) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_RESET, (uint16_t)p);
                kprintf("[USB HUB] Port %d: reset complete\n", p);
            }

            /* Update cached status */
            hub->port_status[p - 1] = status;
        }
    }

    return changes;
}
