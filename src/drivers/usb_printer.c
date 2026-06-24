/*
 * usb_printer.c — USB Printer Class driver
 *
 * Implements USB printer class (bInterfaceClass=7) support.
 * Provides a /dev/usb/lp0 character device for userspace access.
 * Bulk OUT endpoint for data transfer, Bulk IN endpoint for
 * device status (IEEE 1284 status byte).
 *
 * Item S45 — USB printer class driver
 */

#include "usb.h"
#include "devfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"

/* ── USB Printer class constants ────────────────────────────────── */
#define USB_CLASS_PRINTER        0x07
#define USB_PRINTER_SUBCLASS     0x01    /* Printer */
#define USB_PRINTER_PROTO_UNIDIR 0x01    /* Uni-directional */
#define USB_PRINTER_PROTO_BIDIR  0x02    /* Bi-directional */
#define USB_PRINTER_PROTO_1284   0x03    /* IEEE 1284.4 compatible */

/* Printer status bits (IEEE 1284) */
#define PRINTER_STATUS_PAPER_EMPTY  0x20
#define PRINTER_STATUS_SELECTED     0x10
#define PRINTER_STATUS_NO_ERROR     0x08
#define PRINTER_STATUS_BUSY         0x40
#define PRINTER_STATUS_FAULT        0x08   /* inverted: 0 = fault */

/* Printer class-specific requests */
#define PRINTER_REQ_GET_DEVICE_ID   0x00
#define PRINTER_REQ_GET_STATUS      0x01
#define PRINTER_REQ_SOFT_RESET      0x02

/* ── Printer device instance ─────────────────────────────────────── */
#define MAX_PRINTERS 4

struct usb_printer {
    uint8_t  dev_addr;            /* USB device address */
    uint16_t vendor_id;
    uint16_t product_id;

    uint8_t  bulk_out_ep;         /* Bulk OUT endpoint address */
    uint8_t  bulk_in_ep;          /* Bulk IN endpoint address */
    uint8_t  protocol;            /* Printer protocol */

    uint8_t  status;              /* Cached IEEE 1284 status */

    char     device_id[256];      /* IEEE 1284 device ID string */
    int      device_id_len;

    spinlock_t lock;
    int      present;
    int      open_count;
};

static struct usb_printer g_printers[MAX_PRINTERS];
static int g_printer_count = 0;

/* ── Device ID string parsing ────────────────────────────────────── */

/* Parse IEEE 1284 Device ID (MFG:xxx;MDL:xxx;CMD:xxx;...) */
static __attribute__((unused)) void parse_device_id(struct usb_printer *p, const uint8_t *data, int len)
{
    if (!data || len <= 0) return;

    /* IEEE 1284 Device ID is typically a length-prefixed string:
     *   [2 bytes: length (little-endian)] [ASCII key:value; pairs]
     */
    int id_len = 0;
    if (len >= 2) {
        id_len = (int)data[0] | ((int)data[1] << 8);
        int offset = 2;
        if (id_len > len - 2) id_len = len - 2;
        if ((size_t)id_len > sizeof(p->device_id) - 1)
            id_len = (int)sizeof(p->device_id) - 1;
        memcpy(p->device_id, data + offset, (size_t)id_len);
        p->device_id[id_len] = '\0';
    }
    p->device_id_len = id_len;

    kprintf("[PRN] Device ID: %s\n", p->device_id);
}

/* ── Public API for USB subsystem ────────────────────────────────── */

int usb_printer_register(uint8_t dev_addr, uint16_t vid, uint16_t pid,
                          uint8_t bulk_out, uint8_t bulk_in, uint8_t protocol)
{
    if (g_printer_count >= MAX_PRINTERS)
        return -ENOSPC;

    struct usb_printer *p = &g_printers[g_printer_count];
    memset(p, 0, sizeof(*p));

    p->dev_addr = dev_addr;
    p->vendor_id = vid;
    p->product_id = pid;
    p->bulk_out_ep = bulk_out;
    p->bulk_in_ep = bulk_in;
    p->protocol = protocol;
    p->present = 1;
    p->status = PRINTER_STATUS_NO_ERROR | PRINTER_STATUS_SELECTED;
    spinlock_init(&p->lock);

    kprintf("[PRN] USB printer registered: VID=0x%04x PID=0x%04x "
            "addr=%d OUT=0x%02x IN=0x%02x proto=%d\n",
            vid, pid, dev_addr, bulk_out, bulk_in, protocol);

    /* Register /dev/usb/lpN */
    char devname[32];
    snprintf(devname, sizeof(devname), "usb/lp%d", g_printer_count);
    devfs_register_device(devname, (void *)(uintptr_t)(g_printer_count + 1),
                           NULL, NULL);  /* R/W handled by USB stack */

    g_printer_count++;
    return g_printer_count - 1;
}

void usb_printer_unregister(int idx)
{
    if (idx < 0 || idx >= g_printer_count)
        return;

    struct usb_printer *p = &g_printers[idx];
    p->present = 0;

    char devname[32];
    snprintf(devname, sizeof(devname), "usb/lp%d", idx);
    devfs_unregister_device(devname);

    kprintf("[PRN] USB printer %d unregistered\n", idx);
}

/* ── Data transfer (submitted to USB core for actual URB processing) ── */

int usb_printer_write(int idx, const uint8_t *data, uint32_t len)
{
    if (idx < 0 || idx >= g_printer_count)
        return -ENODEV;

    struct usb_printer *p = &g_printers[idx];
    if (!p->present)
        return -ENODEV;

    /* In a full implementation, this submits a bulk OUT URB.
     * For now, this is a stub that returns success after logging. */
    kprintf("[PRN] lp%d: write %u bytes\n", idx, len);
    return (int)len;
}

int usb_printer_read_status(int idx)
{
    if (idx < 0 || idx >= g_printer_count)
        return -ENODEV;

    struct usb_printer *p = &g_printers[idx];
    if (!p->present)
        return -ENODEV;

    /* Submit a bulk IN request to read 1-byte status.
     * For now, return cached status. */
    return (int)p->status;
}

/* ── Class-specific requests (sent via control endpoint) ─────────── */

int usb_printer_get_device_id(int idx, char *buf, int buf_size)
{
    if (idx < 0 || idx >= g_printer_count)
        return -ENODEV;

    struct usb_printer *p = &g_printers[idx];
    if (!p->present || !buf) return -ENODEV;

    if (p->device_id_len > 0) {
        int copy = p->device_id_len < buf_size - 1 ? p->device_id_len : buf_size - 1;
        memcpy(buf, p->device_id, (size_t)copy);
        buf[copy] = '\0';
        return copy;
    }
    return 0;
}

int usb_printer_soft_reset(int idx)
{
    if (idx < 0 || idx >= g_printer_count)
        return -ENODEV;

    struct usb_printer *p = &g_printers[idx];
    if (!p->present) return -ENODEV;

    /* In a full implementation, send class-specific request:
     *   bmRequestType = 0x21 (Class, Interface, Host-to-Device)
     *   bRequest = PRINTER_REQ_SOFT_RESET (0x02)
     *   wIndex = interface number
     */
    kprintf("[PRN] lp%d: soft reset\n", idx);
    return 0;
}

/* ── Utility ─────────────────────────────────────────────────────── */

int usb_printer_get_count(void)
{
    return g_printer_count;
}

/* ── Stub: usb_printer_read ─────────────────────────────── */
int usb_printer_read(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[USB] usb_printer_read: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_printer_get_status ─────────────────────────────── */
int usb_printer_get_status(void *dev)
{
    (void)dev;
    kprintf("[USB] usb_printer_get_status: not yet implemented\n");
    return 0;
}
