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
#include "usb_core.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "delay.h"

/* ── Forward declarations ─────────────────────────────────────────── */
int usb_hub_init(void);

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

/* ── Root hub port state (EHCI root port polling) ──────────────── */

/* Port Status/Control register (relative to EHCI op_base) */
#define EHCI_PORTSC_BASE    0x44

#define ROOT_HUB_MAX_PORTS  8

/* Per-port root hub tracking state */
struct root_hub_port_state {
    int     connected;           /* 1 if a device is present */
    int     core_idx;            /* USB core device index, -1 if none */
    int     debounce_pending;
    uint64_t debounce_until;
};

static struct {
    struct root_hub_port_state ports[ROOT_HUB_MAX_PORTS];
    int n_ports;
    int initialized;
} g_root_hub;

/* ── Hotplug device-port mapping ────────────────────────────────── */
/*
 * Maps USB core device indices back to hub+port for disconnect cleanup.
 * hub_id < 0 = root hub port; hub_id >= 0 = downstream hub index.
 */
#define HOTPLUG_MAX_DEVICES 16

struct hotplug_entry {
    int hub_id;     /* -1 for root hub, 0+ for downstream hub index */
    int port;       /* 1-based port number */
    int core_idx;   /* USB core device index */
    int valid;
};

static struct hotplug_entry g_hotplug[HOTPLUG_MAX_DEVICES];

/* ── Forward declarations for static functions ───────────────────── */
static int hub_port_debounce(struct hub_state *hub, int port, uint16_t status);
static int hub_port_over_current_recover(struct hub_state *hub, int port);

/* ── Hotplug helpers ────────────────────────────────────────────── */

/* Add a device to the hotplug tracking table */
static int hotplug_add(int hub_id, int port, int core_idx)
{
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (!g_hotplug[i].valid) {
            g_hotplug[i].hub_id = hub_id;
            g_hotplug[i].port = port;
            g_hotplug[i].core_idx = core_idx;
            g_hotplug[i].valid = 1;
            return 0;
        }
    }
    return -1;
}

/* Find a hotplug entry by hub+port */
static struct hotplug_entry *hotplug_find(int hub_id, int port)
{
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (g_hotplug[i].valid && g_hotplug[i].hub_id == hub_id &&
            g_hotplug[i].port == port)
            return &g_hotplug[i];
    }
    return NULL;
}

/* Remove a hotplug entry */
static void hotplug_remove(int hub_id, int port)
{
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (g_hotplug[i].valid && g_hotplug[i].hub_id == hub_id &&
            g_hotplug[i].port == port) {
            g_hotplug[i].valid = 0;
            return;
        }
    }
}

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

/* ── Root hub port register access ──────────────────────────────── */

static inline uint32_t root_port_read(int port)
{
    if (port < 0 || port >= g_root_hub.n_ports)
        return 0;
    return op_rd(EHCI_PORTSC_BASE + (uint32_t)port * 4);
}

static inline void root_port_write(int port, uint32_t val)
{
    if (port < 0 || port >= g_root_hub.n_ports)
        return;
    op_wr(EHCI_PORTSC_BASE + (uint32_t)port * 4, val);
}

/* ── Root hub port debounce ─────────────────────────────────────── */
/*
 * Returns 1 when stable, 0 while debouncing, negative on error.
 */
static int root_port_debounce(int port, int connected)
{
    uint64_t now = timer_get_ticks();
    const uint64_t debounce_ms = 100;

    if (!g_root_hub.ports[port].debounce_pending) {
        g_root_hub.ports[port].debounce_pending = 1;
        g_root_hub.ports[port].debounce_until = now + (debounce_ms * 100 / 1000);
        return 0;
    }

    if (now < g_root_hub.ports[port].debounce_until)
        return 0;

    /* Debounce period elapsed -- re-check connection */
    g_root_hub.ports[port].debounce_pending = 0;
    uint32_t portsc = root_port_read(port);
    int still_connected = !!(portsc & 1u);  /* bit 0 = CCS */

    if (still_connected != connected)
        return -1;

    return 1;
}

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
            hub->dev_addr, port,
            speed == 2 ? "high" : (speed == 1 ? "low" : "full"));
}

/* ── Hotplug connect/disconnect handlers ────────────────────────── */

/*
 * hotplug_handle_connect -- Full device enumeration + USB core registration.
 *
 * Handles port reset, descriptor read, and registration with the USB core
 * device model (triggers driver probe via usb_core_add_device).
 *
 * @hub_id:   hub index (-1 for root hub, 0+ for downstream hub)
 * @port:     1-based port number
 * @is_root:  1 if root hub port, 0 if downstream hub port
 * Returns 0 on success, negative on error.
 */
static int hotplug_handle_connect(int hub_id, int port, int is_root)
{
    struct usb_device dev_desc;
    int speed;
    uint8_t dev_addr;
    int rc;

    memset(&dev_desc, 0, sizeof(dev_desc));

    if (is_root) {
        /* Root hub port: reset via EHCI PORTSC register */
        uint32_t portsc = root_port_read(port);
        portsc &= ~(1u << 2);  /* clear PED */
        root_port_write(port, (portsc & ~(1u << 8)) | (1u << 8));  /* set PR */
        busy_wait(500000);   /* 50 ms reset */
        root_port_write(port, portsc & ~(1u << 8));  /* clear PR */
        busy_wait(50000);    /* 5 ms recovery */

        portsc = root_port_read(port);
        if (!(portsc & (1u << 2)))  /* PED not set */
            return -1;

        uint32_t spd = (portsc >> 20) & 3u;
        speed = (spd == 0) ? 2 : 0;  /* 0=HS, 1=FS */
    } else {
        /* Downstream hub port: use hub control requests */
        struct hub_state *hub = &g_hubs[hub_id];

        rc = hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_RESET,
                                  (uint16_t)port);
        if (rc < 0) return rc;
        busy_wait(500000);
        hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_RESET,
                          (uint16_t)port);
        busy_wait((uint32_t)hub->power_good_delay * 2000);

        uint16_t status = 0, change = 0;
        hub_get_port_status(hub->dev_addr, (uint8_t)port, &status, &change);
        if (!(status & PORT_STATUS_CONNECTION) || !(status & PORT_STATUS_ENABLE))
            return -1;

        speed = 0;
        if (status & PORT_STATUS_HIGH_SPEED) speed = 2;
        else if (status & PORT_STATUS_LOW_SPEED) speed = 1;
    }

    /* Assign a USB bus address above the init-time range */
    {
        static uint8_t g_next_addr = 32;
        dev_addr = g_next_addr++;
    }

    /* Read device descriptor to get VID:PID and class */
    uint8_t *desc_buf = (uint8_t *)hub_alloc_dma(18);
    if (!desc_buf) return -1;

    rc = usb_control(dev_addr, 0x80, 0x06, 0x0100, 0, 18, desc_buf);
    if (rc == 0) {
        dev_desc.vendor_id  = (uint16_t)desc_buf[8] | ((uint16_t)desc_buf[9] << 8);
        dev_desc.product_id = (uint16_t)desc_buf[10] | ((uint16_t)desc_buf[11] << 8);
        dev_desc.class_code = desc_buf[4];
        dev_desc.subclass   = desc_buf[5];
        dev_desc.protocol   = desc_buf[6];
        dev_desc.flags      = USB_DEV_FLAG_HAS_DESC;
    } else {
        dev_desc.class_code = 0xFF;
    }
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)desc_buf));

    dev_desc.addr  = dev_addr;
    dev_desc.speed = (uint8_t)speed;

    /* Register with USB core device model (triggers driver probe) */
    int core_idx = usb_core_add_device(&dev_desc);
    if (core_idx < 0) {
        kprintf("[USB HOTPLUG] Failed to register device on hub %d port %d\n",
                hub_id, port);
        return core_idx;
    }

    /* Track in hotplug mapping for disconnect cleanup */
    hotplug_add(hub_id, port, core_idx);
    if (is_root)
        g_root_hub.ports[port].core_idx = core_idx;

    kprintf("[USB HOTPLUG] Device connected on hub %d port %d: "
            "%04x:%04x class=%02x (core idx %d, addr %d)\n",
            hub_id, port,
            (unsigned)dev_desc.vendor_id, (unsigned)dev_desc.product_id,
            (unsigned)dev_desc.class_code, core_idx, dev_addr);

    return 0;
}

/*
 * hotplug_handle_disconnect -- Clean up device and unregister from USB core.
 *
 * Finds the device on the given hub+port, unregisters it from the USB core
 * device model (triggers driver disconnect callback), and cleans up tracking.
 */
static void hotplug_handle_disconnect(int hub_id, int port, int is_root)
{
    struct hotplug_entry *entry = hotplug_find(hub_id, port);
    int core_idx;

    if (entry) {
        core_idx = entry->core_idx;
        kprintf("[USB HOTPLUG] Device disconnected on hub %d port %d "
                "(core_idx %d)\n", hub_id, port, core_idx);

        /* Unregister from USB core -- triggers driver disconnect */
        usb_core_remove_device(core_idx);

        /* Remove from tracking */
        hotplug_remove(hub_id, port);
    } else {
        core_idx = -1;
        kprintf("[USB HOTPLUG] Disconnect on untracked hub %d port %d\n",
                hub_id, port);
    }

    if (is_root) {
        g_root_hub.ports[port].connected = 0;
        if (core_idx >= 0)
            g_root_hub.ports[port].core_idx = -1;
    } else {
        if (hub_id >= 0 && hub_id < g_hub_count)
            g_hubs[hub_id].debounce_pending[port - 1] = 0;
    }
}

/* ── Root hub port polling ──────────────────────────────────────── */
/*
 * Poll EHCI root hub ports for connection/disconnection events.
 * Called from usb_hub_poll() and usb_hub_detect().
 */
static void hub_poll_root_hub(void)
{
    if (!g_root_hub.initialized)
        return;

    for (int p = 0; p < g_root_hub.n_ports; p++) {
        uint32_t portsc = root_port_read(p);

        /* Check for connect status change */
        if (!(portsc & (1u << 1)))  /* bit 1 = CSC */
            continue;

        /* Clear the change bit (write back PORTSC with CSC=1) */
        root_port_write(p, portsc | (1u << 1));

        int connected = !!(portsc & 1u);  /* bit 0 = CCS */

        if (connected) {
            kprintf("[USB ROOT] Port %d: device connected, debouncing...\n", p);
            g_root_hub.ports[p].connected = 1;
            root_port_debounce(p, 1);
        } else {
            kprintf("[USB ROOT] Port %d: device disconnected\n", p);
            g_root_hub.ports[p].debounce_pending = 0;
            if (g_root_hub.ports[p].connected) {
                hotplug_handle_disconnect(-1, p, 1);
            }
        }
    }

    /* Check pending debounce timers */
    for (int p = 0; p < g_root_hub.n_ports; p++) {
        if (!g_root_hub.ports[p].debounce_pending)
            continue;

        int db = root_port_debounce(p, g_root_hub.ports[p].connected);
        if (db > 0) {
            /* Debounce stable -- enumerate */
            kprintf("[USB ROOT] Port %d: debounce stable, enumerating...\n", p);
            hotplug_handle_connect(-1, p, 1);
        } else if (db < 0) {
            kprintf("[USB ROOT] Port %d: debounce failed\n", p);
            g_root_hub.ports[p].debounce_pending = 0;
            if (g_root_hub.ports[p].connected) {
                hotplug_handle_disconnect(-1, p, 1);
            }
        }
    }
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

    /* Validate bNbrPorts: must be between 1 and USB_MAX_PORTS_PER_HUB */
    if (desc->bNbrPorts == 0 || desc->bNbrPorts > USB_MAX_PORTS_PER_HUB) {
        kprintf("[USB HUB] Hub addr=%d: invalid bNbrPorts=%d (max %d)\n",
                dev_addr, desc->bNbrPorts, USB_MAX_PORTS_PER_HUB);
        pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
        return -3;
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
        hub_set_port_feature(dev_addr, HUB_FEATURE_PORT_POWER, (uint16_t)p);
        hub->port_powered[p - 1] = 1;
    }

    /* Wait for power to stabilize */
    busy_wait((uint32_t)hub->power_good_delay * 2000);

    /* Enumerate already-connected devices */
    for (int p = 1; p <= hub->n_ports && p <= USB_MAX_PORTS_PER_HUB; p++) {
        uint16_t status = 0, change = 0;
        hub_get_port_status(dev_addr, (uint8_t)p, &status, &change);

        if (status & PORT_STATUS_CONNECTION) {
            hub_enum_device(hub, (uint8_t)p);
        }
    }

    g_hub_count++;
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)buf));
    return 0;
}

/* ── Periodic hub polling ──────────────────────────────────────────── */

static void usb_hub_poll(void) {
    if (!g_hub_initialized) return;

    /* Poll root hub ports for hotplug events */
    hub_poll_root_hub();

    for (int h = 0; h < g_hub_count; h++) {
        struct hub_state *hub = &g_hubs[h];

        for (int p = 1; p <= hub->n_ports && p <= USB_MAX_PORTS_PER_HUB; p++) {
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
                    /* Use hotplug disconnect for proper cleanup */
                    hotplug_handle_disconnect(h, p, 0);
                }
            }

            /* If we had a pending debounce (not already processed this iteration) */
            if (hub->debounce_pending[p - 1] && !(change & PORT_CHANGE_C_CONNECTION)) {
                int db = hub_port_debounce(hub, p, status);
                if (db > 0) {
                    /* Debounce passed — connection is stable */
                    kprintf("[USB HUB] Port %d: debounce stable, hot-enumerating...\n", p);
                    hotplug_handle_connect(h, p, 0);
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

            /* Suspend change — handle wake from suspend with reset-resume */
            if (change & PORT_CHANGE_C_SUSPEND) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_SUSPEND, (uint16_t)p);

                if (!(status & PORT_STATUS_SUSPEND)) {
                    /* Port resumed from suspend — perform reset-resume
                     * (USB 2.0 §11.5.1, §11.9.1).  A reset-resume ensures the
                     * device is fully recovered to a known operational state
                     * after waking from suspend. */
                    kprintf("[USB HUB] Port %d: wake from suspend, "
                            "reset-resume\n", p);
                    hub_set_port_feature(hub->dev_addr,
                                         HUB_FEATURE_PORT_RESET, (uint16_t)p);
                    mdelay(50);
                    hub_clear_feature(hub->dev_addr,
                                      HUB_FEATURE_C_PORT_RESET, (uint16_t)p);

                    /* Re-read port status after reset */
                    uint16_t rstatus = 0, rchange = 0;
                    hub_get_port_status(hub->dev_addr, (uint8_t)p,
                                        &rstatus, &rchange);
                    if (rstatus & PORT_STATUS_ENABLE)
                        kprintf("[USB HUB] Port %d: reset-resume complete, "
                                "device active\n", p);
                } else {
                    kprintf("[USB HUB] Port %d: suspended\n", p);
                }
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

    /* Initialise root hub polling from EHCI port count */
    memset(&g_root_hub, 0, sizeof(g_root_hub));
    g_root_hub.n_ports = ehci_get_n_ports();
    if (g_root_hub.n_ports > ROOT_HUB_MAX_PORTS)
        g_root_hub.n_ports = ROOT_HUB_MAX_PORTS;
    g_root_hub.initialized = 1;
    kprintf("[USB ROOT] Root hub: %d ports\n", g_root_hub.n_ports);

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
static int usb_hub_port_reset(void *hub_ptr, int port)
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
static int usb_hub_port_enable(void *hub_ptr, int port)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    /* Set PORT_ENABLE feature to enable the port */
    return hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
}

/* ── Hub port disable ───────────────────────────────────────── */
static int usb_hub_port_disable(void *hub_ptr, int port)
{
    struct hub_state *hub = (struct hub_state *)hub_ptr;
    if (!hub || port < 0 || port > hub->n_ports)
        return -1;

    /* Clear PORT_ENABLE to disable the port */
    return hub_clear_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE, (uint16_t)port);
}

/* ── Hub port power control ─────────────────────────────────── */
static int usb_hub_port_power(void *hub_ptr, int port, int on)
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

/* ── Hub port over-current recovery (with debounce) ──────────── */
/*
 * Per USB 2.0 spec §11.8.3, software must debounce the over-current
 * indication before re-enabling the port:
 *   1) Clear C_PORT_OVER_CURRENT
 *   2) Poll wPortStatus — verify PORT_STATUS_OVER_CURRENT is clear
 *   3) If the condition persists, wait and retry (debounce)
 *   4) Only re-enable the port after the condition clears
 *   5) If a reasonable timeout expires, leave the port disabled.
 */
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

    /* Debounce: poll port status until over-current clears or timeout.
     * Use a 1-second total timeout with 100ms polling intervals. */
    uint64_t deadline = timer_get_ticks() + (1000 * 100 / 1000);  /* 1 sec */

    do {
        /* Wait 100 ms between checks */
        mdelay(100);

        uint16_t status = 0, change = 0;
        rc = hub_get_port_status(hub->dev_addr, (uint8_t)port, &status, &change);
        if (rc < 0)
            return rc;

        /* Over-current condition cleared — re-enable the port */
        if (!(status & PORT_STATUS_OVER_CURRENT)) {
            rc = hub_set_port_feature(hub->dev_addr, HUB_FEATURE_PORT_ENABLE,
                                      (uint16_t)port);
            if (rc == 0) {
                kprintf("[USB HUB] Port %d: over-current cleared, "
                        "port re-enabled\n", port);
            }
            return rc;
        }

        /* Clear any new over-current change flags that appeared during the wait */
        if (change & PORT_CHANGE_C_OVER_CURRENT) {
            hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_OVER_CURRENT,
                              (uint16_t)port);
        }

        /* Check for timeout */
        if (timer_get_ticks() >= deadline) {
            kprintf("[USB HUB] Port %d: over-current condition persists, "
                    "leaving port disabled\n", port);
            return -1;
        }
    } while (1);
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

    /*
     * If a new connect/disconnect change flag appeared in the bitmap during
     * software debounce, a transient disconnection/reconnection occurred.
     * Leave C_PORT_CONNECTION set so the next poll cycle re-processes it
     * and starts a fresh debounce (USB 2.0 spec §7.1.7.3).
     */
    if (final_change & PORT_CHANGE_C_CONNECTION) {
        kprintf("[USB HUB] Port %d: connection change during debounce, "
                "restarting\n", port);
        return 0;
    }

    if (!!(final_status & PORT_STATUS_CONNECTION) != !!(status & PORT_STATUS_CONNECTION)) {
        /* State changed during debounce — restart */
        kprintf("[USB HUB] Port %d: connection state changed during debounce, "
                "restarting\n", port);
        return 0;  /* caller will re-detect on next poll */
    }

    return 1;  /* Connection stable */
}

/* ── Detect port status changes on all hubs ─────────────────── */
static int usb_hub_detect(void)
{
    int changes = 0;

    /* Check root hub ports for changes */
    hub_poll_root_hub();
    if (g_root_hub.initialized)
        changes += g_root_hub.n_ports;  /* at least polled */

    for (int h = 0; h < g_hub_count; h++) {
        struct hub_state *hub = &g_hubs[h];

        for (int p = 1; p <= hub->n_ports && p <= USB_MAX_PORTS_PER_HUB; p++) {
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
                if (status & PORT_STATUS_CONNECTION) {
                    kprintf("[USB HUB] Port %d: connection change (connected)\n", p);
                    /* Start debounce */
                    (void)hub_port_debounce(hub, p, status);
                } else {
                    kprintf("[USB HUB] Port %d: device disconnected\n", p);
                    hotplug_handle_disconnect(h, p, 0);
                }
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

            /* Handle suspend changes — perform reset-resume on wake */
            if (change & PORT_CHANGE_C_SUSPEND) {
                hub_clear_feature(hub->dev_addr, HUB_FEATURE_C_PORT_SUSPEND, (uint16_t)p);

                if (!(status & PORT_STATUS_SUSPEND)) {
                    kprintf("[USB HUB] Port %d: wake from suspend, "
                            "reset-resume\n", p);
                    hub_set_port_feature(hub->dev_addr,
                                         HUB_FEATURE_PORT_RESET, (uint16_t)p);
                    mdelay(50);
                    hub_clear_feature(hub->dev_addr,
                                      HUB_FEATURE_C_PORT_RESET, (uint16_t)p);

                    /* Re-read port status after reset */
                    uint16_t rstatus = 0, rchange = 0;
                    hub_get_port_status(hub->dev_addr, (uint8_t)p,
                                        &rstatus, &rchange);
                    if (rstatus & PORT_STATUS_ENABLE)
                        kprintf("[USB HUB] Port %d: reset-resume complete, "
                                "device active\n", p);
                } else {
                    kprintf("[USB HUB] Port %d: suspended\n", p);
                }
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
