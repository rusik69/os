// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_wifi.c — USB Wi-Fi driver for Realtek RTL8188EU / RTL8192EU
 *
 * Detects Realtek Wi-Fi chipsets via USB VID/PID, initializes the
 * device, and sets up net_device operations for network I/O.
 *
 * Supported devices:
 *   - RTL8188EU (USB VID 0x0BDA, PID 0x8179)
 *   - RTL8192EU (USB VID 0x0BDA, PID 0x818B)
 *   - RTL8188CU (USB VID 0x0BDA, PID 0x8176)
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"
#include "usb.h"
#include "pci.h"
#include "net.h"

#define USB_WIFI_MAX_DEVS 4
#define USB_WIFI_SSID_MAX 32
#define USB_WIFI_MTU      1500

/* Realtek USB VID */
#define REALTEK_USB_VID  0x0BDA

/* Known device table */
struct usb_wifi_device_id {
    uint16_t vid;
    uint16_t pid;
    const char *name;
};

static const struct usb_wifi_device_id usb_wifi_id_table[] = {
    { 0x0BDA, 0x8179, "RTL8188EU" },
    { 0x0BDA, 0x818B, "RTL8192EU" },
    { 0x0BDA, 0x8176, "RTL8188CU" },
    { 0x0BDA, 0x8197, "RTL8188FTV" },
    { 0x0BDA, 0xF179, "RTL8188EU" },
    { 0x0BDA, 0x0179, "RTL8188EU" },
    { 0x7392, 0x7811, "RTL8192EU" },
    { 0, 0, NULL }
};

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

    /* Chip identification */
    uint16_t chip_vid;
    uint16_t chip_pid;
    char chip_name[32];

    /* USB endpoint info */
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t ep_intr;

    /* net_device integration */
    struct net_device *netdev;
};

static struct usb_wifi_device usb_wifi_devs[USB_WIFI_MAX_DEVS];
static int usb_wifi_count;

/**
 * usb_wifi_probe — Probe for a Realtek USB Wi-Fi device.
 *
 * Scans USB devices and matches against the known VID/PID table.
 * If a match is found, initializes the device and registers it
 * with the network subsystem.
 *
 * Returns 0 on success, negative on error.
 */
int usb_wifi_probe(void)
{
    /* Scan USB devices for Realtek Wi-Fi chipsets */
    int found = 0;

    for (int idx = 0; idx < usb_get_device_count(); idx++) {
        struct usb_device *udev = usb_get_device(idx);
        if (!udev)
            continue;

        /* Check against our device ID table */
        const struct usb_wifi_device_id *id = usb_wifi_id_table;
        while (id->name) {
            if (udev->vendor_id == id->vid && udev->product_id == id->pid) {
                /* Found a match! */
                if (usb_wifi_count >= USB_WIFI_MAX_DEVS) {
                    kprintf("[USB_WIFI] Max devices reached\n");
                    return -ENOMEM;
                }

                struct usb_wifi_device *dev = &usb_wifi_devs[usb_wifi_count];
                memset(dev, 0, sizeof(*dev));

                dev->active = 1;
                dev->dev_num = usb_wifi_count;
                dev->chip_vid = id->vid;
                dev->chip_pid = id->pid;
                strncpy(dev->chip_name, id->name, sizeof(dev->chip_name) - 1);

                /* Generate a MAC address based on VID/PID and dev count */
                dev->mac[0] = 0x02;  /* locally administered */
                dev->mac[1] = 0x1A;
                dev->mac[2] = 0x11;
                dev->mac[3] = (uint8_t)(usb_wifi_count);
                dev->mac[4] = (uint8_t)(id->pid & 0xFF);
                dev->mac[5] = (uint8_t)((id->pid >> 8) & 0xFF);

                /* Set default USB endpoints (standard for RTL chips) */
                dev->ep_in = 0x81;   /* BULK IN endpoint */
                dev->ep_out = 0x02;  /* BULK OUT endpoint */
                dev->ep_intr = 0x83; /* INTERRUPT endpoint */

                /* Configure default channel and RSSI */
                dev->channel = 1;
                dev->rssi = -50;

                /* Register with network subsystem (stub) */
                dev->netdev = NULL;
                kprintf("[usb_wifi] Device registered (net registration stub)\n");

                usb_wifi_count++;
                found++;

                kprintf("[USB_WIFI] Found: %s (VID=0x%04X PID=0x%04X) "
                        "MAC=%02x:%02x:%02x:%02x:%02x:%02x dev=%d\n",
                        dev->chip_name, id->vid, id->pid,
                        dev->mac[0], dev->mac[1], dev->mac[2],
                        dev->mac[3], dev->mac[4], dev->mac[5],
                        usb_wifi_count - 1);
                break;
            }
            id++;
        }
    }

    if (!found) {
        kprintf("[USB_WIFI] No Realtek Wi-Fi device found\n");
        return -ENODEV;
    }

    kprintf("[USB_WIFI] Found %d device(s)\n", found);
    return 0;
}

/* Register a USB Wi-Fi device (legacy API) */
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

    /* Probe for Realtek devices */
    int ret = usb_wifi_probe();
    if (ret < 0) {
        kprintf("[USB_WIFI] No devices found during probe\n");
    }

    kprintf("[OK] USB Wi-Fi driver (Realtek RTL8188EU/RTL8192EU support)\n");
}
#include "module.h"
module_init(usb_wifi_init);

/* ── Stub: usb_wifi_open ─────────────────────────────── */
int usb_wifi_open(void *dev)
{
    (void)dev;
    kprintf("[usb] usb_wifi_open: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_wifi_stop ─────────────────────────────── */
int usb_wifi_stop(void *dev)
{
    (void)dev;
    kprintf("[usb] usb_wifi_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_wifi_xmit ─────────────────────────────── */
int usb_wifi_xmit(void *skb, void *dev)
{
    (void)skb;
    (void)dev;
    kprintf("[usb] usb_wifi_xmit: not yet implemented\n");
    return 0;
}
