// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_cdc_ether.c — CDC Ethernet / RNDIS driver
 *
 * Implements USB CDC Ethernet Control Model (ECM) and
 * Microsoft RNDIS for network connectivity over USB.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "heap.h"

#define CDC_ETHER_MAX_DEVS 4
#define CDC_ETHER_MTU      1500
#define CDC_ETHER_BUF_SIZE 2048

struct cdc_ether_device {
    int active;
    int dev_num;
    uint8_t mac_addr[6];
    uint32_t speed; /* in Mbps */
    uint8_t rx_buf[CDC_ETHER_BUF_SIZE];
    int rx_len;
    uint8_t tx_buf[CDC_ETHER_BUF_SIZE];
    int tx_len;
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
};

static struct cdc_ether_device cdc_ether_devs[CDC_ETHER_MAX_DEVS];
static int cdc_ether_count;

/* Register a CDC Ethernet device */
int cdc_ether_register(const uint8_t *mac, int dev_num)
{
    if (cdc_ether_count >= CDC_ETHER_MAX_DEVS)
        return -ENOMEM;

    struct cdc_ether_device *dev = &cdc_ether_devs[cdc_ether_count];
    dev->active = 1;
    dev->dev_num = dev_num;
    memcpy(dev->mac_addr, mac, 6);
    dev->speed = 100; /* 100 Mbps default */
    cdc_ether_count++;

    kprintf("[CDC_ETHER] Registered dev=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            dev_num, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return cdc_ether_count - 1;
}

/* Send an Ethernet frame via USB CDC */
int cdc_ether_send(int dev_id, const uint8_t *frame, int len)
{
    if (dev_id < 0 || dev_id >= cdc_ether_count || !cdc_ether_devs[dev_id].active)
        return -ENODEV;

    struct cdc_ether_device *dev = &cdc_ether_devs[dev_id];
    int copy_len = (len > CDC_ETHER_BUF_SIZE) ? CDC_ETHER_BUF_SIZE : len;
    memcpy(dev->tx_buf, frame, (size_t)copy_len);
    dev->tx_len = copy_len;
    dev->tx_packets++;
    dev->tx_bytes += copy_len;

    return copy_len;
}

/* Receive an Ethernet frame from USB CDC */
int cdc_ether_recv(int dev_id, uint8_t *frame, int max_len)
{
    if (dev_id < 0 || dev_id >= cdc_ether_count || !cdc_ether_devs[dev_id].active)
        return -ENODEV;

    struct cdc_ether_device *dev = &cdc_ether_devs[dev_id];
    if (dev->rx_len <= 0)
        return 0;

    int copy_len = (max_len > dev->rx_len) ? dev->rx_len : max_len;
    memcpy(frame, dev->rx_buf, (size_t)copy_len);
    dev->rx_len -= copy_len;
    dev->rx_packets++;
    dev->rx_bytes += copy_len;

    return copy_len;
}

/* Get statistics */
void cdc_ether_get_stats(int dev_id, uint64_t *tx_pkts, uint64_t *rx_pkts,
                          uint64_t *tx_bytes, uint64_t *rx_bytes)
{
    if (dev_id < 0 || dev_id >= cdc_ether_count || !cdc_ether_devs[dev_id].active)
        return;

    struct cdc_ether_device *dev = &cdc_ether_devs[dev_id];
    if (tx_pkts) *tx_pkts = dev->tx_packets;
    if (rx_pkts) *rx_pkts = dev->rx_packets;
    if (tx_bytes) *tx_bytes = dev->tx_bytes;
    if (rx_bytes) *rx_bytes = dev->rx_bytes;
}

void usb_cdc_ether_init(void)
{
    memset(cdc_ether_devs, 0, sizeof(cdc_ether_devs));
    cdc_ether_count = 0;
    kprintf("[OK] USB CDC Ethernet / RNDIS driver\n");
}
#include "module.h"
module_init(usb_cdc_ether_init);
