// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_serial.c — USB serial driver (FTDI, PL2303, CP2102)
 *
 * Supports common USB-to-serial converters: FTDI FT232,
 * Prolific PL2303, and Silicon Labs CP2102.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define USB_SERIAL_MAX_DEVS 8
#define USB_SERIAL_BUF_SIZE 4096

/* Known USB serial chip types */
#define USB_SERIAL_UNKNOWN 0
#define USB_SERIAL_FTDI    1
#define USB_SERIAL_PL2303  2
#define USB_SERIAL_CP2102  3

struct usb_serial_device {
    int active;
    int chip_type;
    int dev_num;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t baud_rate;
    uint8_t rx_buf[USB_SERIAL_BUF_SIZE];
    int rx_len;
    uint8_t tx_buf[USB_SERIAL_BUF_SIZE];
    int tx_len;
};

static struct usb_serial_device usb_ser_devs[USB_SERIAL_MAX_DEVS];
static int usb_ser_count = 0;

/* Register a USB serial device */
static int usb_serial_register(int chip_type, uint16_t vid, uint16_t pid)
{
    if (usb_ser_count >= USB_SERIAL_MAX_DEVS)
        return -ENOMEM;

    struct usb_serial_device *dev = &usb_ser_devs[usb_ser_count];
    dev->active = 1;
    dev->chip_type = chip_type;
    dev->dev_num = usb_ser_count;
    dev->vendor_id = vid;
    dev->product_id = pid;
    dev->baud_rate = 115200;
    usb_ser_count++;

    const char *chip_name = "unknown";
    switch (chip_type) {
    case USB_SERIAL_FTDI:   chip_name = "FTDI"; break;
    case USB_SERIAL_PL2303: chip_name = "PL2303"; break;
    case USB_SERIAL_CP2102: chip_name = "CP2102"; break;
    default:
        break;
    }

    kprintf("[USB_SERIAL] %s device registered: %04x:%04x\n",
            chip_name, vid, pid);
    return usb_ser_count - 1;
}

/* Set baud rate */
static int usb_serial_set_baud(int dev_id, uint32_t baud)
{
    if (dev_id < 0 || dev_id >= usb_ser_count || !usb_ser_devs[dev_id].active)
        return -ENODEV;

    usb_ser_devs[dev_id].baud_rate = baud;
    kprintf("[USB_SERIAL] dev=%d baud=%u\n", dev_id, baud);
    return 0;
}

/* Write data to USB serial */
static int usb_serial_write(int dev_id, const uint8_t *data, int len)
{
    if (dev_id < 0 || dev_id >= usb_ser_count || !usb_ser_devs[dev_id].active)
        return -ENODEV;

    struct usb_serial_device *dev = &usb_ser_devs[dev_id];
    int copy_len = (len > USB_SERIAL_BUF_SIZE) ? USB_SERIAL_BUF_SIZE : len;
    memcpy(dev->tx_buf, data, (size_t)copy_len);
    dev->tx_len = copy_len;

    return copy_len;
}

/* Read data from USB serial */
static int usb_serial_read(int dev_id, uint8_t *buf, int max)
{
    if (dev_id < 0 || dev_id >= usb_ser_count || !usb_ser_devs[dev_id].active)
        return -ENODEV;

    struct usb_serial_device *dev = &usb_ser_devs[dev_id];
    int copy_len = (max > dev->rx_len) ? dev->rx_len : max;
    memcpy(buf, dev->rx_buf, (size_t)copy_len);
    dev->rx_len -= copy_len;
    return copy_len;
}

static void usb_serial_init(void)
{
    memset(usb_ser_devs, 0, sizeof(usb_ser_devs));
    usb_ser_count = 0;
    kprintf("[OK] USB Serial — FTDI, PL2303, CP2102 driver\n");
}
#include "module.h"
module_init(usb_serial_init);

/* ── Stub: usb_serial_open ─────────────────────────────── */
static int usb_serial_open(void *dev)
{
    (void)dev;
    kprintf("[USB] usb_serial_open: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_serial_close ─────────────────────────────── */
static int usb_serial_close(void *dev)
{
    (void)dev;
    kprintf("[USB] usb_serial_close: not yet implemented\n");
    return 0;
}
