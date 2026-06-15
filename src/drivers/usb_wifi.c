// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_wifi.c — USB Wi-Fi skeleton
 *
 * Skeleton driver for USB Wi-Fi adapters supporting common chipsets.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define USB_WIFI_MAX_DEVS 4
#define USB_WIFI_SSID_MAX 32
#define USB_WIFI_MTU      1500

struct usb_wifi_device {
    int active;
    int dev_num;
    char ssid[USB_WIFI_SSID_MAX + 1];
    uint8_t mac[6];
    int channel;
    int rssi;
    int connected;
    uint8_t rx_buf[2048];
    int rx_len;
    uint8_t tx_buf[2048];
    int tx_len;
};

static struct usb_wifi_device usb_wifi_devs[USB_WIFI_MAX_DEVS];
static int usb_wifi_count;

/* Register a USB Wi-Fi device */
int usb_wifi_register(int dev_num, const uint8_t *mac)
{
    if (usb_wifi_count >= USB_WIFI_MAX_DEVS)
        return -ENOMEM;

    struct usb_wifi_device *dev = &usb_wifi_devs[usb_wifi_count];
    dev->active = 1;
    dev->dev_num = dev_num;
    memcpy(dev->mac, mac, 6);
    dev->channel = 1;
    dev->rssi = -50;
    dev->connected = 0;
    usb_wifi_count++;

    kprintf("[USB_WIFI] Registered dev=%d\n", dev_num);
    return usb_wifi_count - 1;
}

/* Connect to a Wi-Fi network */
int usb_wifi_connect(int dev_id, const char *ssid, const char *password)
{
    if (dev_id < 0 || dev_id >= usb_wifi_count || !usb_wifi_devs[dev_id].active)
        return -ENODEV;

    struct usb_wifi_device *dev = &usb_wifi_devs[dev_id];
    strncpy(dev->ssid, ssid, USB_WIFI_SSID_MAX);
    dev->connected = 1;
    (void)password;

    kprintf("[USB_WIFI] Connected to '%s'\n", ssid);
    return 0;
}

/* Disconnect */
int usb_wifi_disconnect(int dev_id)
{
    if (dev_id < 0 || dev_id >= usb_wifi_count || !usb_wifi_devs[dev_id].active)
        return -ENODEV;

    usb_wifi_devs[dev_id].connected = 0;
    kprintf("[USB_WIFI] Disconnected\n");
    return 0;
}

/* Send data frame */
int usb_wifi_send(int dev_id, const uint8_t *data, int len)
{
    if (dev_id < 0 || dev_id >= usb_wifi_count || !usb_wifi_devs[dev_id].active)
        return -ENODEV;

    struct usb_wifi_device *dev = &usb_wifi_devs[dev_id];
    if (!dev->connected) return -ENOTCONN;

    int copy_len = (len > 2048) ? 2048 : len;
    memcpy(dev->tx_buf, data, (size_t)copy_len);
    dev->tx_len = copy_len;

    return copy_len;
}

/* Receive data frame */
int usb_wifi_recv(int dev_id, uint8_t *buf, int max)
{
    if (dev_id < 0 || dev_id >= usb_wifi_count || !usb_wifi_devs[dev_id].active)
        return -ENODEV;

    struct usb_wifi_device *dev = &usb_wifi_devs[dev_id];
    if (dev->rx_len <= 0) return 0;

    int copy_len = (max > dev->rx_len) ? dev->rx_len : max;
    memcpy(buf, dev->rx_buf, (size_t)copy_len);
    dev->rx_len -= copy_len;

    return copy_len;
}

void usb_wifi_init(void)
{
    memset(usb_wifi_devs, 0, sizeof(usb_wifi_devs));
    usb_wifi_count = 0;
    kprintf("[OK] USB Wi-Fi driver skeleton\n");
}
#include "module.h"
module_init(usb_wifi_init);
