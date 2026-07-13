// SPDX-License-Identifier: GPL-2.0-only
/*
 * usb_cdc_ether.c — USB CDC Ethernet Control Model (ECM) / EEM driver
 *
 * Implements Ethernet-over-USB using the CDC Ethernet Control Model
 * (ECM, subclass 0x06) and Ethernet Emulation Model (EEM, subclass 0x07).
 * Provides the control model layer: Ethernet functional descriptor
 * parsing, packet filter management, multicast filter management,
 * and Ethernet statistics collection.
 *
 * References:
 *   USB Class Definitions for Communication Devices, v1.2 (§3.8)
 *   CDC Ethernet Control Model (ECM), v1.2
 *   CDC Ethernet Emulation Model (EEM), v1.0
 */
#define KERNEL_INTERNAL
#include "usb.h"
#include "usb_core.h"
#include "netdevice.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "heap.h"
#include "module.h"

/* ── CDC class constants ──────────────────────────────────────────── */

#define CDC_CLASS_COMM         0x02

/* Subclass codes */
#define CDC_SUBCLASS_ECM       0x06   /* Ethernet Control Model */
#define CDC_SUBCLASS_EEM       0x07   /* Ethernet Emulation Model */

/* CDC functional descriptor subtypes (within the Communication class) */
#define CDC_DTYPE_HEADER       0x00
#define CDC_DTYPE_CM           0x01   /* Call Management */
#define CDC_DTYPE_ACM          0x02   /* Abstract Control Management */
#define CDC_DTYPE_UNION        0x06   /* Interface Union */
#define CDC_DTYPE_ETHERNET     0x0F   /* Ethernet Functional Descriptor */
#define CDC_DTYPE_NCM          0x1A   /* Network Control Model */

/* CDC ECM class-specific requests (bmRequestType = 0x21) */
#define CDC_SEND_ENCAPSULATED_COMMAND    0x00
#define CDC_GET_ENCAPSULATED_RESPONSE    0x01
#define CDC_SET_ETHERNET_PACKET_FILTER   0x43
#define CDC_SET_ETHERNET_MULTICAST_FILTERS 0x42
#define CDC_SET_ETHERNET_POWER_MGMT_PATTERN 0x45
#define CDC_GET_ETHERNET_POWER_MGMT_PATTERN 0x46
#define CDC_GET_ETHERNET_STATISTIC       0x44
#define CDC_GET_NTB_PARAMETERS           0x80

/* ── Ethernet Packet Filter flags ────────────────────────────────── */

#define PACKET_TYPE_PROMISCUOUS   (1U << 0)
#define PACKET_TYPE_ALL_MULTICAST (1U << 1)
#define PACKET_TYPE_DIRECTED      (1U << 2)
#define PACKET_TYPE_BROADCAST     (1U << 3)
#define PACKET_TYPE_MULTICAST     (1U << 4)

/* ── Ethernet Statistics Selector Codes (CDC ECM §6.3) ─────────────── */

#define STAT_XMIT_OK              0x01   /* frames transmitted OK */
#define STAT_RCV_OK               0x02   /* frames received OK */
#define STAT_XMIT_ERROR           0x03   /* frames transmit errors */
#define STAT_RCV_ERROR            0x04   /* frames receive errors */
#define STAT_RCV_NO_BUFFER        0x05   /* no buffer errors (receive) */
#define STAT_DIRECTED_BYTES_XMIT  0x06   /* directed bytes transmitted */
#define STAT_DIRECTED_BYTES_RCV   0x07   /* directed bytes received */
#define STAT_MCAST_BYTES_XMIT     0x08   /* multicast bytes transmitted */
#define STAT_MCAST_BYTES_RCV      0x09   /* multicast bytes received */
#define STAT_BCAST_BYTES_XMIT     0x0A   /* broadcast bytes transmitted */
#define STAT_BCAST_BYTES_RCV      0x0B   /* broadcast bytes received */
#define STAT_DIRECTED_FRAMES_XMIT 0x0C   /* directed frames transmitted */
#define STAT_DIRECTED_FRAMES_RCV  0x0D   /* directed frames received */
#define STAT_MCAST_FRAMES_XMIT    0x0E   /* multicast frames transmitted */
#define STAT_MCAST_FRAMES_RCV     0x0F   /* multicast frames received */
#define STAT_BCAST_FRAMES_XMIT    0x10   /* broadcast frames transmitted */
#define STAT_BCAST_FRAMES_RCV     0x11   /* broadcast frames received */
#define STAT_RCV_CRC_ERROR        0x12   /* receive CRC/alignment errors */
#define STAT_TRANSMIT_QUEUE_LEN   0x13   /* transmit queue length */
#define STAT_RCV_ERROR_ALIGNMENT  0x14   /* receive alignment errors */
#define STAT_XMIT_ONE_COLLISION   0x15   /* one collision on transmit */
#define STAT_XMIT_MORE_COLLISIONS 0x16   /* multiple collisions on transmit */

/* ── EEM protocol constants ──────────────────────────────────────── */

#define EEM_HEADER_TYPE_DATA    0   /* bit 15 = 0: data packet */
#define EEM_HEADER_TYPE_CMD    1   /* bit 15 = 1: command packet */
#define EEM_HEADER_TYPE_SHIFT  15

#define EEM_HEADER_CRC         (1U << 14)  /* CRC present after data */
#define EEM_HEADER_RESERVED    (7U << 11)  /* reserved bits */

/* Data packet: bits 11-0 = total length (including EEM header) */
#define EEM_DATA_LENGTH_MASK   0x07FF

/* Command packet: bits 14-12 = cmd type, bits 11-0 = cmd data */
#define EEM_CMD_ECHO           0   /* Echo request/response */
#define EEM_CMD_SUSPEND        1   /* Suspend hint */
#define EEM_CMD_ECHO_RESPONSE  2   /* Echo response */
#define EEM_CMD_GET_CRC_HEADER 3   /* Request CRC awareness */
#define EEM_CMD_SET_CRC_HEADER 4   /* Set CRC awareness */
#define EEM_CMD_ECHO_HEADER    5   /* Echo with header */

/* ── Driver limits ───────────────────────────────────────────────── */

#define CDC_ETHER_MAX_DEVS      4
#define CDC_ETHER_MTU           1500
#define CDC_ETHER_BUF_SIZE      2048
#define CDC_ETHER_MAX_MCAST     32   /* max multicast filter entries */
#define CDC_ETHER_MAX_PM_PAT    8    /* max power management patterns */

/* ── CDC Ethernet Functional Descriptor (CDC ECM §5.4) ────────────── */

/*
 * This descriptor appears within the Communication interface's
 * class-specific descriptors and provides MAC address and
 * capability information.
 */
struct __attribute__((packed)) cdc_ether_descriptor {
    uint8_t  bLength;             /* 13 bytes for ECM (or more) */
    uint8_t  bDescriptorType;     /* 0x24 (CS_INTERFACE) */
    uint8_t  bDescriptorSubType;  /* 0x0F (CDC_ETH) */
    uint8_t  iMACAddress;         /* string index for MAC address */
    uint32_t bmEthernetStatistics;/* supported statistics bitmap */
    uint16_t wMaxSegmentSize;     /* max segment size */
    uint16_t wNumberMCFilters;    /* number of multicast filters */
    uint8_t  bNumberPowerFilters; /* power management filter count */
};

/* ── Ethernet statistics (per-device) ────────────────────────────── */

struct cdc_ether_stats {
    uint64_t xmit_ok;
    uint64_t rcv_ok;
    uint64_t xmit_error;
    uint64_t rcv_error;
    uint64_t rcv_no_buffer;
    uint64_t directed_bytes_xmit;
    uint64_t directed_bytes_rcv;
    uint64_t mcast_bytes_xmit;
    uint64_t mcast_bytes_rcv;
    uint64_t bcast_bytes_xmit;
    uint64_t bcast_bytes_rcv;
    uint64_t directed_frames_xmit;
    uint64_t directed_frames_rcv;
    uint64_t mcast_frames_xmit;
    uint64_t mcast_frames_rcv;
    uint64_t bcast_frames_xmit;
    uint64_t bcast_frames_rcv;
    uint64_t rcv_crc_error;
    uint64_t transmit_queue_len;
};

/* ── Per-device private data ─────────────────────────────────────── */

struct cdc_ether_device {
    int              active;
    int              dev_num;
    uint8_t          mac_addr[6];
    uint32_t         speed;          /* in Mbps */
    uint32_t         packet_filter;  /* current packet filter flags */

    /* ECM descriptor data */
    uint32_t         eth_stats_cap;  /* supported statistics bitmap */
    uint16_t         max_seg_size;   /* wMaxSegmentSize */
    uint16_t         num_mc_filters; /* wNumberMCFilters */
    uint8_t          num_pm_filters; /* bNumberPowerFilters */

    /* Multicast filter list */
    uint8_t          mc_filters[CDC_ETHER_MAX_MCAST][6];
    uint16_t         mc_filter_count;

    /* TX/RX buffers */
    uint8_t          rx_buf[CDC_ETHER_BUF_SIZE];
    int              rx_len;
    uint8_t          tx_buf[CDC_ETHER_BUF_SIZE];
    int              tx_len;

    /* Statistics */
    struct cdc_ether_stats stats;

    /* Device model: 0 = ECM, 1 = EEM */
    int              is_eem;

    /* Lock */
    spinlock_t       lock;
};

/* ── Global state ────────────────────────────────────────────────── */

static struct cdc_ether_device cdc_ether_devs[CDC_ETHER_MAX_DEVS];
static int cdc_ether_count = 0;
static int g_initialized = 0;

/* Supported statistics bitmap (what we can report) */
#define CDC_ETH_STATS_SUPPORTED \
    ( (1U << (STAT_XMIT_OK - 1)) | \
      (1U << (STAT_RCV_OK - 1)) | \
      (1U << (STAT_XMIT_ERROR - 1)) | \
      (1U << (STAT_RCV_ERROR - 1)) | \
      (1U << (STAT_RCV_NO_BUFFER - 1)) | \
      (1U << (STAT_DIRECTED_BYTES_XMIT - 1)) | \
      (1U << (STAT_DIRECTED_BYTES_RCV - 1)) | \
      (1U << (STAT_DIRECTED_FRAMES_XMIT - 1)) | \
      (1U << (STAT_DIRECTED_FRAMES_RCV - 1)) | \
      (1U << (STAT_BCAST_BYTES_XMIT - 1)) | \
      (1U << (STAT_BCAST_BYTES_RCV - 1)) | \
      (1U << (STAT_XMIT_ONE_COLLISION - 1)) | \
      (1U << (STAT_XMIT_MORE_COLLISIONS - 1)) )

/* ── Internal helpers ────────────────────────────────────────────── */

static struct cdc_ether_device *cdc_ether_get_dev(int dev_id)
{
    if (dev_id < 0 || dev_id >= cdc_ether_count)
        return NULL;
    struct cdc_ether_device *dev = &cdc_ether_devs[dev_id];
    if (!dev->active)
        return NULL;
    return dev;
}

/*
 * Build a 2-byte EEM header for a data packet.
 * For data packets (bit 15 = 0), bits 11-0 carry the total
 * length of the EEM packet including the header itself.
 */
static uint16_t eem_build_data_header(uint16_t total_len)
{
    if (total_len > EEM_DATA_LENGTH_MASK)
        total_len = EEM_DATA_LENGTH_MASK;
    return total_len & EEM_DATA_LENGTH_MASK;
}

/*
 * Parse a 2-byte EEM header.
 * Returns 0 for data packets, 1 for command packets.
 * @out_len: receives the payload length (excluding EEM header)
 * @out_crc: receives CRC flag
 */
static int eem_parse_header(uint16_t header, uint16_t *out_len, int *out_crc)
{
    int type = (header >> EEM_HEADER_TYPE_SHIFT) & 1;
    if (out_crc)
        *out_crc = (header & EEM_HEADER_CRC) ? 1 : 0;
    /* Both data and command packets encode payload info in bits 11-0 */
    if (out_len)
        *out_len = header & EEM_DATA_LENGTH_MASK;
    return type;
}

/* ── CDC ECM control requests ────────────────────────────────────── */

/*
 * Send a CDC class-specific request to the USB device.
 * bmRequestType: 0x21 (host-to-device, class, interface)
 * For device-to-host: 0xA1
 */
static int cdc_ether_control(uint8_t bm_req_type, uint8_t b_request,
                             uint16_t w_value, uint16_t w_index,
                             uint16_t w_length, void *data)
{
    /* In simulation mode, we handle requests locally.
     * On real hardware, this would submit a control URB to EP0. */
    (void)bm_req_type;
    (void)b_request;
    (void)w_value;
    (void)w_index;
    (void)w_length;
    (void)data;
    return 0;
}

/*
 * Set the Ethernet packet filter for a device.
 * The host sends this to configure which packets the device
 * should accept (unicast, broadcast, multicast, promiscuous).
 */
static int cdc_set_ether_packet_filter(struct cdc_ether_device *dev,
                                       uint16_t filter_flags)
{
    if (!dev)
        return -EINVAL;

    spinlock_acquire(&dev->lock);
    dev->packet_filter = filter_flags;
    spinlock_release(&dev->lock);

    kprintf("[CDC_ECM] Dev %d: packet filter = 0x%04x\n",
            dev->dev_num, filter_flags);

    return cdc_ether_control(0x21, CDC_SET_ETHERNET_PACKET_FILTER,
                             filter_flags, 0, 0, NULL);
}

/*
 * Set multicast filters (list of Ethernet multicast MAC addresses).
 */
static int cdc_set_multicast_filters(struct cdc_ether_device *dev,
                                     const uint8_t *filters,
                                     uint16_t count)
{
    if (!dev || !filters)
        return -EINVAL;
    if (count > CDC_ETHER_MAX_MCAST)
        count = CDC_ETHER_MAX_MCAST;

    spinlock_acquire(&dev->lock);
    dev->mc_filter_count = count;
    for (uint16_t i = 0; i < count; i++)
        memcpy(dev->mc_filters[i], filters + i * 6, 6);
    spinlock_release(&dev->lock);

    kprintf("[CDC_ECM] Dev %d: %u multicast filters\n",
            dev->dev_num, count);

    return cdc_ether_control(0x21, CDC_SET_ETHERNET_MULTICAST_FILTERS,
                             0, 0, (uint16_t)(count * 6),
                             (void *)(uintptr_t)filters);
}

/*
 * Get an Ethernet statistic value.
 * @stat_selector: STAT_XMIT_OK, STAT_RCV_OK, etc.
 * @out: receives the 64-bit statistic value
 */
static int cdc_get_ether_statistic(struct cdc_ether_device *dev,
                                   uint8_t stat_selector,
                                   uint64_t *out)
{
    if (!dev || !out)
        return -EINVAL;

    uint64_t val = 0;

    spinlock_acquire(&dev->lock);

    switch (stat_selector) {
    case STAT_XMIT_OK:
        val = dev->stats.xmit_ok;
        break;
    case STAT_RCV_OK:
        val = dev->stats.rcv_ok;
        break;
    case STAT_XMIT_ERROR:
        val = dev->stats.xmit_error;
        break;
    case STAT_RCV_ERROR:
        val = dev->stats.rcv_error;
        break;
    case STAT_RCV_NO_BUFFER:
        val = dev->stats.rcv_no_buffer;
        break;
    case STAT_DIRECTED_BYTES_XMIT:
        val = dev->stats.directed_bytes_xmit;
        break;
    case STAT_DIRECTED_BYTES_RCV:
        val = dev->stats.directed_bytes_rcv;
        break;
    case STAT_MCAST_BYTES_XMIT:
        val = dev->stats.mcast_bytes_xmit;
        break;
    case STAT_MCAST_BYTES_RCV:
        val = dev->stats.mcast_bytes_rcv;
        break;
    case STAT_BCAST_BYTES_XMIT:
        val = dev->stats.bcast_bytes_xmit;
        break;
    case STAT_BCAST_BYTES_RCV:
        val = dev->stats.bcast_bytes_rcv;
        break;
    case STAT_DIRECTED_FRAMES_XMIT:
        val = dev->stats.directed_frames_xmit;
        break;
    case STAT_DIRECTED_FRAMES_RCV:
        val = dev->stats.directed_frames_rcv;
        break;
    case STAT_MCAST_FRAMES_XMIT:
        val = dev->stats.mcast_frames_xmit;
        break;
    case STAT_MCAST_FRAMES_RCV:
        val = dev->stats.mcast_frames_rcv;
        break;
    case STAT_BCAST_FRAMES_XMIT:
        val = dev->stats.bcast_frames_xmit;
        break;
    case STAT_BCAST_FRAMES_RCV:
        val = dev->stats.bcast_frames_rcv;
        break;
    case STAT_RCV_CRC_ERROR:
        val = dev->stats.rcv_crc_error;
        break;
    case STAT_TRANSMIT_QUEUE_LEN:
        val = dev->stats.transmit_queue_len;
        break;
    default:
        spinlock_release(&dev->lock);
        return -EINVAL;
    }

    spinlock_release(&dev->lock);

    if (out)
        *out = val;

    return 0;
}

/* ── EEM command processing ──────────────────────────────────────── */

static int eem_process_command(struct cdc_ether_device *dev,
                               uint16_t cmd_data)
{
    uint8_t cmd_type = (uint8_t)((cmd_data >> 12) & 0x07);
    uint16_t cmd_val = cmd_data & 0x0FFF;

    switch (cmd_type) {
    case EEM_CMD_ECHO:
        /* Echo request — respond with echo response */
        kprintf("[CDC_EEM] Dev %d: echo cmd=0x%03x\n",
                dev->dev_num, cmd_val);
        break;

    case EEM_CMD_SUSPEND:
        /* Suspend hint — host may be suspending */
        kprintf("[CDC_EEM] Dev %d: suspend hint\n", dev->dev_num);
        break;

    case EEM_CMD_ECHO_RESPONSE:
        /* Echo response */
        break;

    case EEM_CMD_GET_CRC_HEADER:
        /* Query CRC awareness — we don't use CRC headers */
        break;

    case EEM_CMD_SET_CRC_HEADER:
        /* Set CRC awareness */
        break;

    default:
        kprintf("[CDC_EEM] Dev %d: unknown cmd %u\n",
                dev->dev_num, cmd_type);
        return -EINVAL;
    }

    return 0;
}

/* ── CDC Ethernet Functional Descriptor parsing ──────────────────── */

/*
 * Parse the CDC Ethernet Functional Descriptor from a config descriptor.
 * Extracts MAC address string index, statistics capabilities, and
 * filter limits.
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_parse_ether_descriptor(const uint8_t *config_data,
                                      uint16_t config_len,
                                      uint8_t *out_mac_idx,
                                      uint32_t *out_stats_cap,
                                      uint16_t *out_max_seg,
                                      uint16_t *out_num_mc,
                                      uint8_t *out_num_pm)
{
    int pos = 0;

    if (!config_data || config_len < 9)
        return -EINVAL;

    /* Skip the config descriptor header (9 bytes) */
    pos += 9;

    while (pos + 2 < (int)config_len) {
        uint8_t dlen = config_data[pos];
        uint8_t dtype = config_data[pos + 1];

        if (dlen == 0)
            break;
        if (pos + dlen > (int)config_len)
            break;

        /* Class-specific descriptor (CS_INTERFACE = 0x24) */
        if (dtype == 0x24 && dlen >= 13) {
            uint8_t sub = config_data[pos + 2];

            if (sub == CDC_DTYPE_ETHERNET) {
                const struct cdc_ether_descriptor *eth_desc =
                    (const struct cdc_ether_descriptor *)(config_data + pos);

                if (out_mac_idx)
                    *out_mac_idx = eth_desc->iMACAddress;
                if (out_stats_cap)
                    *out_stats_cap = eth_desc->bmEthernetStatistics;
                if (out_max_seg)
                    *out_max_seg = eth_desc->wMaxSegmentSize;
                if (out_num_mc)
                    *out_num_mc = eth_desc->wNumberMCFilters;
                if (out_num_pm)
                    *out_num_pm = eth_desc->bNumberPowerFilters;

                return 0;
            }
        }

        pos += dlen;
    }

    return -ENOENT;
}

/*
 * Check whether the device at @dev_addr is CDC ECM (subclass 0x06)
 * or CDC EEM (subclass 0x07).
 * Returns 0 on success, negative on failure.
 */
static int cdc_detect_model(uint8_t dev_addr, int *is_eem)
{
    (void)dev_addr;
    /* In simulation, default to ECM.
     * On real hardware, we'd parse the config descriptor to find
     * the Communication interface's subclass. */
    if (is_eem)
        *is_eem = 0;
    return 0;
}

/*
 * Read the MAC address from the USB device.
 * First tries the CDC Ethernet Functional Descriptor's string index.
 * If that fails, generates a locally-administered MAC from vendor/product.
 */
static int cdc_read_mac_address(uint8_t dev_addr,
                                const uint8_t *config_data,
                                uint16_t config_len,
                                uint8_t *mac_out)
{
    uint8_t mac_idx = 0;

    /* Try to get MAC from the Ethernet functional descriptor string */
    int rc = cdc_parse_ether_descriptor(config_data, config_len,
                                        &mac_idx, NULL, NULL, NULL, NULL);
    if (rc == 0 && mac_idx != 0) {
        /* On real hardware, read the string descriptor:
         *   GET_DESCRIPTOR(USB_DT_STRING, mac_idx, ...)
         * The string contains the MAC in hex "xx:xx:xx:xx:xx:xx" or
         * as 6 raw bytes in Unicode format.
         *
         * In simulation, we generate a MAC from device address.
         */
        (void)dev_addr;
        mac_out[0] = 0x02;
        mac_out[1] = 0x00;
        mac_out[2] = 0x00;
        mac_out[3] = 0x00;
        mac_out[4] = 0x00;
        mac_out[5] = (uint8_t)dev_addr;
        return 0;
    }

    /* Fallback: locally-administered MAC */
    mac_out[0] = 0x02;
    mac_out[1] = 0x00;
    mac_out[2] = 0x00;
    mac_out[3] = 0x00;
    mac_out[4] = 0x00;
    mac_out[5] = (uint8_t)dev_addr;
    return 0;
}

/* ── Data path (ECM framing) ────────────────────────────────────────── */

/*
 * Prepare an Ethernet frame for transmission.
 * For ECM: send the raw Ethernet frame.
 * For EEM: prepend a 2-byte EEM data header.
 * @dev: the CDC ethernet device
 * @frame: input Ethernet frame
 * @len: input frame length
 * @out_buf: output buffer (must be CDC_ETHER_BUF_SIZE)
 * @out_len: receives output length
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_frame_tx(struct cdc_ether_device *dev,
                              const uint8_t *frame, uint16_t len,
                              uint8_t *out_buf, uint16_t *out_len)
{
    if (!dev || !frame || !out_buf || !out_len)
        return -EINVAL;
    if (len > CDC_ETHER_MTU)
        return -EMSGSIZE;

    if (dev->is_eem) {
        /* EEM: prepend 2-byte header */
        uint16_t total_len = len + 2;
        if (total_len > CDC_ETHER_BUF_SIZE)
            return -ENOBUFS;

        uint16_t header = eem_build_data_header(total_len);
        out_buf[0] = (uint8_t)(header & 0xFF);
        out_buf[1] = (uint8_t)(header >> 8);
        memcpy(out_buf + 2, frame, len);
        *out_len = total_len;
    } else {
        /* ECM: raw Ethernet frame */
        if (len > CDC_ETHER_BUF_SIZE)
            return -ENOBUFS;
        memcpy(out_buf, frame, len);
        *out_len = len;
    }

    return 0;
}

/*
 * Extract an Ethernet frame from a received buffer.
 * For ECM: the buffer is the raw Ethernet frame.
 * For EEM: strip the 2-byte EEM header (handle command packets too).
 * @out_frame_len: receives the Ethernet frame length extracted
 * Returns the number of bytes consumed from the input buffer on success,
 *         negative errno on failure.
 */
static int cdc_ether_frame_rx(struct cdc_ether_device *dev,
                              const uint8_t *in_buf, uint16_t in_len,
                              uint8_t *out_frame, uint16_t max_out,
                              uint16_t *out_frame_len)
{
    if (!dev || !in_buf || !out_frame)
        return -EINVAL;

    if (dev->is_eem && in_len >= 2) {
        /* EEM: parse the 2-byte header */
        uint16_t header = (uint16_t)in_buf[0] | ((uint16_t)in_buf[1] << 8);
        uint16_t payload_len;
        int crc_flag;
        int type = eem_parse_header(header, &payload_len, &crc_flag);

        if (type == EEM_HEADER_TYPE_CMD) {
            /* Command packet — process and return 0 consumed (no data) */
            eem_process_command(dev, header & 0x0FFF);
            if (out_frame_len)
                *out_frame_len = 0;
            return (int)payload_len;  /* total EEM packet consumed */
        }

        /* Data packet: payload follows the 2-byte header */
        uint16_t eth_len;
        if (payload_len >= 2) {
            eth_len = payload_len - 2; /* subtract EEM header */
        } else {
            return -EINVAL;
        }

        /* Account for optional CRC trailer (4 bytes) */
        if (crc_flag && eth_len >= 4)
            eth_len -= 4;

        if (eth_len > max_out)
            eth_len = max_out;

        uint16_t copy_len = (in_len - 2 < eth_len) ? (in_len - 2) : eth_len;
        memcpy(out_frame, in_buf + 2, copy_len);
        if (out_frame_len)
            *out_frame_len = copy_len;
        /* Return total bytes consumed from input (the full EEM packet) */
        return (int)payload_len;
    }

    /* ECM: raw frame */
    uint16_t copy_len = (in_len < max_out) ? in_len : max_out;
    memcpy(out_frame, in_buf, copy_len);
    if (out_frame_len)
        *out_frame_len = copy_len;
    return (int)copy_len;
}

/* ── Public API (used by the netdevice layer and users of this driver) ── */

/*
 * Register a CDC Ethernet device with the given MAC address.
 * Returns the device ID (>= 0) on success, negative errno on failure.
 */
static int cdc_ether_register(const uint8_t *mac, int dev_num)
{
    if (!mac)
        return -EINVAL;
    if (cdc_ether_count >= CDC_ETHER_MAX_DEVS)
        return -ENOMEM;

    struct cdc_ether_device *dev = &cdc_ether_devs[cdc_ether_count];
    memset(dev, 0, sizeof(*dev));
    spinlock_init(&dev->lock);

    dev->active = 1;
    dev->dev_num = dev_num;
    memcpy(dev->mac_addr, mac, 6);
    dev->speed = 100;           /* 100 Mbps default */
    dev->packet_filter = PACKET_TYPE_DIRECTED | PACKET_TYPE_BROADCAST;
    dev->max_seg_size = CDC_ETHER_MTU;
    dev->eth_stats_cap = CDC_ETH_STATS_SUPPORTED;
    dev->is_eem = 0;            /* default to ECM */

    cdc_ether_count++;

    kprintf("[CDC_ECM] Registered dev=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x, "
            "speed=%u Mbps\n",
            dev_num, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            (unsigned)dev->speed);
    return cdc_ether_count - 1;
}

/*
 * Send an Ethernet frame via USB CDC.
 * For ECM: transmits the raw frame.
 * For EEM: adds the 2-byte EEM header before transmission.
 * Returns the number of bytes transmitted on success, negative errno.
 */
static int cdc_ether_send(int dev_id, const uint8_t *frame, int len)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;
    if (!frame || len <= 0)
        return -EINVAL;

    uint16_t out_len = 0;
    int rc = cdc_ether_frame_tx(dev, frame, (uint16_t)len,
                                 dev->tx_buf, &out_len);
    if (rc < 0)
        return rc;

    dev->tx_len = (int)out_len;

    spinlock_acquire(&dev->lock);

    dev->stats.xmit_ok++;
    if ((dev->packet_filter & PACKET_TYPE_DIRECTED) ||
        !(dev->packet_filter & PACKET_TYPE_PROMISCUOUS)) {
        /* Count directed bytes */
        if (len >= 12) {
            int is_mcast = (frame[0] & 1);
            int is_bcast = (frame[0] == 0xFF && frame[1] == 0xFF &&
                            frame[2] == 0xFF && frame[3] == 0xFF &&
                            frame[4] == 0xFF && frame[5] == 0xFF);
            if (is_bcast) {
                dev->stats.bcast_bytes_xmit += len;
                dev->stats.bcast_frames_xmit++;
            } else if (is_mcast) {
                dev->stats.mcast_bytes_xmit += len;
                dev->stats.mcast_frames_xmit++;
            } else {
                dev->stats.directed_bytes_xmit += len;
                dev->stats.directed_frames_xmit++;
            }
        }
    }

    spinlock_release(&dev->lock);

    return (int)out_len;
}

/*
 * Receive an Ethernet frame from USB CDC.
 * For ECM: returns the raw frame.
 * For EEM: strips the EEM header.
 * Returns the number of bytes received (0 = none), negative errno on error.
 */
static int cdc_ether_recv(int dev_id, uint8_t *frame, int max_len)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;
    if (!frame)
        return -EINVAL;
    if (dev->rx_len <= 0)
        return 0;

    uint16_t in_len = (uint16_t)dev->rx_len;
    uint16_t frame_len = 0;
    int consumed = cdc_ether_frame_rx(dev, dev->rx_buf, in_len,
                                 frame, (uint16_t)max_len,
                                 &frame_len);
    if (consumed < 0)
        return consumed;
    if (frame_len == 0)
        return 0;

    spinlock_acquire(&dev->lock);

    dev->stats.rcv_ok++;
    if (frame_len >= 12) {
        int is_mcast = (frame[0] & 1);
        int is_bcast = (frame[0] == 0xFF && frame[1] == 0xFF &&
                        frame[2] == 0xFF && frame[3] == 0xFF &&
                        frame[4] == 0xFF && frame[5] == 0xFF);
        if (is_bcast) {
            dev->stats.bcast_bytes_rcv += frame_len;
            dev->stats.bcast_frames_rcv++;
        } else if (is_mcast) {
            dev->stats.mcast_bytes_rcv += frame_len;
            dev->stats.mcast_frames_rcv++;
        } else {
            dev->stats.directed_bytes_rcv += frame_len;
            dev->stats.directed_frames_rcv++;
        }
    }

    dev->rx_len -= consumed;
    /* Shift remaining data */
    if (dev->rx_len > 0)
        memmove(dev->rx_buf, dev->rx_buf + consumed, (size_t)dev->rx_len);

    spinlock_release(&dev->lock);

    return frame_len;
}

/*
 * Queue received data into the receive buffer (called from the USB
 * bulk IN completion handler, or simulation equivalent).
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_queue_rx(int dev_id, const uint8_t *data, int len)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;
    if (!data || len <= 0)
        return -EINVAL;

    spinlock_acquire(&dev->lock);

    /* Check for buffer overflow */
    if (dev->rx_len + len > CDC_ETHER_BUF_SIZE) {
        dev->stats.rcv_no_buffer++;
        spinlock_release(&dev->lock);
        return -ENOBUFS;
    }

    memcpy(dev->rx_buf + dev->rx_len, data, (size_t)len);
    dev->rx_len += len;

    spinlock_release(&dev->lock);
    return 0;
}

/* ── Statistics access ────────────────────────────────────────────── */

/*
 * Get device statistics.
 * Fills in the provided pointers with current counter values.
 */
static void cdc_ether_get_stats(int dev_id,
                          uint64_t *tx_pkts, uint64_t *rx_pkts,
                          uint64_t *tx_bytes, uint64_t *rx_bytes)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return;

    spinlock_acquire(&dev->lock);

    if (tx_pkts)  *tx_pkts  = dev->stats.xmit_ok;
    if (rx_pkts)  *rx_pkts  = dev->stats.rcv_ok;
    if (tx_bytes) *tx_bytes = dev->stats.directed_bytes_xmit +
                               dev->stats.mcast_bytes_xmit +
                               dev->stats.bcast_bytes_xmit;
    if (rx_bytes) *rx_bytes = dev->stats.directed_bytes_rcv +
                               dev->stats.mcast_bytes_rcv +
                               dev->stats.bcast_bytes_rcv;

    spinlock_release(&dev->lock);
}

/*
 * Query a specific Ethernet statistic selector.
 * Wrapper around cdc_get_ether_statistic.
 * Returns the 64-bit statistic value, or 0 on error.
 */
static uint64_t cdc_ether_query_stat(int dev_id, uint8_t selector)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return 0;

    uint64_t val = 0;
    int rc = cdc_get_ether_statistic(dev, selector, &val);
    if (rc < 0)
        return 0;
    return val;
}

/* ── Control model API (for integration with netdevice) ────────────── */

/*
 * Initialize the CDC ethernet device's control model.
 * Called during probe to set up initial filter state and
 * read the MAC address and Ethernet Functional Descriptor.
 * @dev_id: device ID from cdc_ether_register()
 * @config_data: configuration descriptor blob for parsing
 * @config_len: total length of the configuration descriptor
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_control_init(int dev_id,
                           const uint8_t *config_data,
                           uint16_t config_len)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;

    /* Parse the Ethernet Functional Descriptor */
    uint32_t stats_cap = 0;
    uint16_t max_seg = 0;
    uint16_t num_mc = 0;
    uint8_t  num_pm = 0;

    int rc = cdc_parse_ether_descriptor(config_data, config_len,
                                        NULL, &stats_cap, &max_seg,
                                        &num_mc, &num_pm);
    if (rc == 0) {
        dev->eth_stats_cap = stats_cap;
        dev->max_seg_size = max_seg;
        dev->num_mc_filters = num_mc;
        dev->num_pm_filters = num_pm;

        kprintf("[CDC_ECM] Dev %d: stats_cap=0x%08x max_seg=%u "
                "mc_filters=%u pm_filters=%u\n",
                dev->dev_num, stats_cap, max_seg,
                num_mc, num_pm);
    }

    /* Set default packet filter (directed + broadcast) */
    cdc_set_ether_packet_filter(dev, PACKET_TYPE_DIRECTED |
                                     PACKET_TYPE_BROADCAST);

    return 0;
}

/*
 * Open the CDC ethernet device (enable RX).
 * Called when the associated netdevice is brought up.
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_open(int dev_id)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;

    kprintf("[CDC_ECM] Dev %d: opening\n", dev->dev_num);

    /* On real hardware, this would:
     * 1. Set the USB configuration
     * 2. Claim the data interface
     * 3. Submit RX URBs on the bulk IN endpoint
     * 4. Start receiving Ethernet frames
     */
    return 0;
}

/*
 * Stop the CDC ethernet device (disable RX).
 * Called when the associated netdevice is brought down.
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_stop(int dev_id)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;

    kprintf("[CDC_ECM] Dev %d: stopping\n", dev->dev_num);

    /* On real hardware, this would:
     * 1. Kill all pending URBs
     * 2. Release the data interface
     * 3. Reset the device's packet filter to zero
     */
    return 0;
}

/*
 * Set the device model (ECM or EEM).
 * @dev_id: device ID
 * @is_eem: 0 for ECM, 1 for EEM
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_set_model(int dev_id, int is_eem)
{
    struct cdc_ether_device *dev = cdc_ether_get_dev(dev_id);
    if (!dev)
        return -ENODEV;

    dev->is_eem = is_eem ? 1 : 0;
    kprintf("[CDC_%s] Dev %d: model set\n",
            is_eem ? "EEM" : "ECM", dev->dev_num);
    return 0;
}

/*
 * Send an Ethernet frame through the CDC ethernet control model.
 * This is the xmit callback entry point for netdevice integration.
 * @dev_id: device ID
 * @skb: network buffer (Ethernet frame)
 * @len: frame length
 * Returns 0 on success, negative errno on failure.
 */
static int cdc_ether_xmit(int dev_id, const void *skb, int len)
{
    return cdc_ether_send(dev_id, (const uint8_t *)skb, len);
}

/*
 * Detect the model (ECM vs EEM) from the configuration descriptor.
 * Scans the Communication interface's subclass.
 * Returns 0 for ECM, 1 for EEM, or -ENOENT if neither found.
 */
static int cdc_detect_ether_model(const uint8_t *config_data, uint16_t config_len)
{
    int pos = 9; /* skip config descriptor header */

    if (!config_data || config_len < 9)
        return -EINVAL;

    while (pos + 2 < (int)config_len) {
        uint8_t dlen = config_data[pos];
        uint8_t dtype = config_data[pos + 1];

        if (dlen == 0)
            break;
        if (pos + dlen > (int)config_len)
            break;

        if (dtype == 4) { /* Interface descriptor */
            uint8_t if_class = config_data[pos + 5];
            uint8_t if_sub   = config_data[pos + 6];

            if (if_class == CDC_CLASS_COMM) {
                if (if_sub == CDC_SUBCLASS_ECM)
                    return 0; /* ECM */
                if (if_sub == CDC_SUBCLASS_EEM)
                    return 1; /* EEM */
            }
        }

        pos += dlen;
    }

    return -ENOENT;
}

/* ── Module initialisation ──────────────────────────────────────────── */

static void __init usb_cdc_ether_init(void)
{
    if (g_initialized)
        return;

    memset(cdc_ether_devs, 0, sizeof(cdc_ether_devs));
    for (int i = 0; i < CDC_ETHER_MAX_DEVS; i++)
        spinlock_init(&cdc_ether_devs[i].lock);

    cdc_ether_count = 0;
    g_initialized = 1;

    kprintf("[OK] USB CDC ECM/EEM ethernet control model\n");
}

static void usb_cdc_ether_exit(void)
{
    for (int i = 0; i < cdc_ether_count; i++) {
        struct cdc_ether_device *dev = &cdc_ether_devs[i];
        if (dev->active) {
            cdc_ether_stop(i);
            dev->active = 0;
        }
    }

    cdc_ether_count = 0;
    g_initialized = 0;
    kprintf("[OK] USB CDC ECM/EEM driver unloaded\n");
}

module_init(usb_cdc_ether_init);
module_exit(usb_cdc_ether_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("USB CDC Ethernet Control Model (ECM) and EEM driver");
MODULE_AUTHOR("OS Kernel Team");
MODULE_ALIAS("usb:v* p*d* d* dc02 dsc06 dp*");          /* CDC ECM */
MODULE_ALIAS("usb:v* p*d* d* dc02 dsc07 dp*");          /* CDC EEM */
