/*
 * usb_hid_consumer.c — USB HID Consumer Page driver (media keys)
 *
 * Implements HID Consumer Page (usage page 0x0C) support for media keys
 * and other consumer controls (volume, play/pause, mute, etc.).
 *
 * The driver parses HID report descriptors to detect Consumer Control
 * application collections, then processes raw input reports to generate
 * press/release events for consumer keys.
 *
 * References:
 *   USB HID Usage Tables, §15 — Consumer Page (0x0C)
 *   USB Device Class Definition for HID, Version 1.11
 *
 * Copyright (c) 2026 Rusik69 OS Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "uhid.h"
#include "usb.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"

/* ── Consumer key state tracking ──────────────────────────────────── */

/*
 * Maximum number of distinct consumer usages we track simultaneously.
 * Consumer reports use 1- or 2-byte usage values; typical keyboards
 * have 5-10 consumer keys at most.
 */
#define CONSUMER_MAX_TRACKED    32

/*
 * Tracked consumer key state — remembers which keys are currently held.
 */
static uint16_t g_tracked_keys[CONSUMER_MAX_TRACKED];
static int      g_num_tracked = 0;

/* Global consumer device state */
static struct hid_consumer_dev g_consumer;
static int g_consumer_initialized = 0;

/* Optional callback for consumer key events */
static void (*g_consumer_callback)(uint16_t code, int pressed) = NULL;

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * Check if a consumer usage is currently tracked as pressed.
 */
static int consumer_is_pressed(uint16_t code)
{
    for (int i = 0; i < g_num_tracked; i++) {
        if (g_tracked_keys[i] == code)
            return 1;
    }
    return 0;
}

/*
 * Add a consumer usage to the tracked-pressed set.
 */
static void consumer_track_press(uint16_t code)
{
    if (g_num_tracked >= CONSUMER_MAX_TRACKED)
        return;
    /* Don't add duplicates */
    if (consumer_is_pressed(code))
        return;
    g_tracked_keys[g_num_tracked++] = code;
}

/*
 * Remove a consumer usage from the tracked-pressed set.
 */
static void consumer_track_release(uint16_t code)
{
    for (int i = 0; i < g_num_tracked; i++) {
        if (g_tracked_keys[i] == code) {
            /* Shift remaining entries down */
            for (int j = i; j < g_num_tracked - 1; j++)
                g_tracked_keys[j] = g_tracked_keys[j + 1];
            g_num_tracked--;
            return;
        }
    }
}

/*
 * Queue a consumer event in the ring buffer.
 */
static void consumer_queue_event(uint16_t code, int pressed)
{
    struct hid_consumer_dev *dev = &g_consumer;

    spinlock_acquire(&dev->lock);

    int next = (dev->ev_tail + 1) % CONSUMER_EVENT_QUEUE;
    if (next != dev->ev_head) {
        dev->events[dev->ev_tail].code    = code;
        dev->events[dev->ev_tail].pressed = pressed;
        dev->ev_tail = next;
    }

    spinlock_release(&dev->lock);

    /* Fire optional callback */
    if (g_consumer_callback)
        g_consumer_callback(code, pressed);
}

/* ── HID report descriptor parser (consumer-specific) ─────────────── */

/*
 * Parse the HID report descriptor to extract consumer usage information.
 *
 * Scans for:
 *  - Collection(Application, Consumer Control) — marks the device as consumer
 *  - Input items within a Consumer Control collection — tracks report size
 *  - Usage items to understand the consumer usages being reported
 *
 * Returns the expected input report length in bytes, or 0 if no consumer
 * control usage is found.
 */
static int consumer_parse_report_desc(const uint8_t *desc, int len,
                                       int *out_report_len)
{
    if (!desc || len <= 0 || !out_report_len)
        return 0;

    int i = 0;
    uint32_t usage_page = 0;
    uint32_t usage = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    uint32_t report_size = 8;
    uint32_t report_count = 0;
    uint32_t logical_min = 0;
    uint32_t logical_max = 255;
    int in_consumer_control = 0;
    int collection_depth = 0;
    int consumer_collections = 0;
    int total_bits = 0;
    int found_consumer = 0;

    while (i < len) {
        uint8_t prefix = desc[i];
        if (prefix == 0) {
            i++;
            continue;
        }

        uint8_t bTag  = (prefix >> 4) & 0x0F;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bSize = prefix & 0x03;

        int data_bytes = (bSize == 3) ? 4 : (int)bSize;
        if (i + 1 + data_bytes > len)
            break;

        i++; /* consume prefix */

        uint32_t data = 0;
        if (data_bytes >= 1) data  = desc[i];
        if (data_bytes >= 2) data |= (uint32_t)desc[i+1] << 8;
        if (data_bytes >= 4) {
            data |= (uint32_t)desc[i+2] << 16;
            data |= (uint32_t)desc[i+3] << 24;
        }

        switch (bType) {
        case 0: /* Main items */
            switch (bTag) {
            case 8:  /* Input */
                if (in_consumer_control && report_count > 0 && report_size > 0) {
                    total_bits += (int)(report_count * report_size);
                    found_consumer = 1;
                }
                /* Reset local state per Main item */
                usage = 0;
                usage_min = 0;
                usage_max = 0;
                report_count = 0;
                break;

            case 9:  /* Output */
            case 11: /* Feature */
                report_count = 0;
                usage = 0;
                usage_min = 0;
                usage_max = 0;
                break;

            case 10: { /* Collection */
                uint8_t coll_type = (uint8_t)(data & 0xFF);
                collection_depth++;

                /* Check if this is a Consumer Control Application Collection */
                if (coll_type == HID_COLLECTION_APPLICATION &&
                    usage_page == HID_PAGE_CONSUMER &&
                    usage == HID_USAGE_CONSUMER_CONTROL) {
                    in_consumer_control = 1;
                    consumer_collections++;
                    kprintf("[CONSUMER] Found Consumer Control collection\n");
                }
                break;
            }

            case 12: /* End Collection */
                if (collection_depth > 0) {
                    collection_depth--;
                    if (collection_depth == 0)
                        in_consumer_control = 0;
                }
                break;
            }
            break;

        case 1: /* Global items */
            switch (bTag) {
            case 0:  /* Usage Page */
                usage_page = data;
                break;
            case 1:  /* Logical Minimum */
                logical_min = data;
                (void)logical_min;
                break;
            case 2:  /* Logical Maximum */
                logical_max = data;
                (void)logical_max;
                break;
            case 7:  /* Report Size */
                report_size = data;
                break;
            case 8:  /* Report ID */
                break;
            case 9:  /* Report Count */
                report_count = data;
                break;
            default:
                break;
            }
            break;

        case 2: /* Local items */
            switch (bTag) {
            case 0:  /* Usage */
                usage = data;
                break;
            case 1:  /* Usage Minimum */
                usage_min = data;
                break;
            case 2:  /* Usage Maximum */
                usage_max = data;
                (void)usage_max;
                break;
            default:
                break;
            }
            break;

        default:
            break;
        }

        i += data_bytes;
    }

    if (found_consumer && total_bits > 0) {
        *out_report_len = (total_bits + 7) / 8;
        kprintf("[CONSUMER] Report length: %d bytes (%d bits)\n",
                *out_report_len, total_bits);
        return 1;
    }

    /* Even without finding Input items, if we saw a Consumer Control
     * collection, assume a default 4-byte report. */
    if (consumer_collections > 0) {
        *out_report_len = 4;
        return 1;
    }

    return 0;
}

/* ── Input report parser ─────────────────────────────────────────── */

/*
 * Extract consumer usages from a raw HID input report.
 *
 * Consumer HID reports typically use a bitmap of 1-bit toggles or
 * array-style usage values.  We use a simple heuristic:
 *
 *   - For 1-bit fields (report_size == 1): each bit represents a toggle
 *     for a specific usage in a usage-min/usage-max range.  If bit is 1,
 *     the corresponding usage is pressed.
 *
 *   - For multi-bit fields (report_size == 8 or 16): the field value
 *     directly encodes a HID usage code or a selector.  Non-zero means
 *     pressed.
 *
 * A more robust implementation would walk the full report descriptor
 * structure, but this covers the vast majority of consumer HID devices.
 */
static void consumer_process_report(const uint8_t *report, int len)
{
    if (!report || len <= 0) return;
    (void)len;

    /* Current implementation: parse the report using the full descriptor.
     * For now, use a simplified approach that handles two common patterns:
     *
     * Pattern A: 1-bit toggle array (e.g., report_count=15, report_size=1)
     *   Usage Minimum = HID_CONSUMER_VOLUME_INCREMENT
     *   Usage Maximum = HID_CONSUMER_MUTE
     *   Each bit position maps to a usage in [min, max] range.
     *
     * Pattern B: 16-bit selector (e.g., report_count=1, report_size=16)
     *   The report value contains a HID_CONSUMER_* usage code.
     *
     * We re-parse the descriptor to extract the exact field layout,
     * then walk each field to detect changes.
     */
    struct hid_consumer_dev *dev = &g_consumer;
    if (!dev->report_desc || dev->report_desc_len <= 0)
        return;

    const uint8_t *desc = dev->report_desc;
    int dlen = dev->report_desc_len;

    /* Walk the descriptor to find Input items within Consumer Control */
    int i = 0;
    uint32_t usage_page = 0;
    uint32_t cur_usage = 0;
    uint32_t cur_usage_min = 0;
    uint32_t cur_usage_max = 0;
    uint32_t cur_report_size = 8;
    uint32_t cur_report_count = 0;
    int in_consumer = 0;
    int collection_depth = 0;
    int report_bit_offset = 0;

    while (i < dlen) {
        uint8_t prefix = desc[i];
        if (prefix == 0) { i++; continue; }

        uint8_t bTag  = (prefix >> 4) & 0x0F;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bSize = prefix & 0x03;

        int data_bytes = (bSize == 3) ? 4 : (int)bSize;
        if (i + 1 + data_bytes > dlen) break;
        i++;

        uint32_t data = 0;
        if (data_bytes >= 1) data  = desc[i];
        if (data_bytes >= 2) data |= (uint32_t)desc[i+1] << 8;
        if (data_bytes >= 4) {
            data |= (uint32_t)desc[i+2] << 16;
            data |= (uint32_t)desc[i+3] << 24;
        }

        switch (bType) {
        case 0:
            switch (bTag) {
            case 8: { /* Input */
                if (!in_consumer || cur_report_count == 0)
                    break;

                uint32_t flags = data;
                int is_constant = (flags & HID_IOF_CONST) ? 1 : 0;
                int is_variable = (flags & HID_IOF_VARIABLE) ? 1 : 0;
                int bit_size = (int)cur_report_size;
                int count = (int)cur_report_count;

                if (is_constant) {
                    /* Constant values don't convey key state */
                    report_bit_offset += bit_size * count;
                    break;
                }

                if (is_variable && bit_size == 1 && cur_usage_max >= cur_usage_min) {
                    /* Pattern A: 1-bit toggle array (bitmap) */
                    uint32_t base_usage = cur_usage_min;
                    for (int b = 0; b < count && b < 64; b++) {
                        int byte_off = report_bit_offset / 8;
                        int bit_off  = report_bit_offset % 8;
                        uint16_t usage_code = (uint16_t)(base_usage + b);

                        int bit_val = 0;
                        if (byte_off < len) {
                            bit_val = (report[byte_off] >> bit_off) & 1;
                        }

                        int was_pressed = consumer_is_pressed(usage_code);
                        if (bit_val && !was_pressed) {
                            consumer_track_press(usage_code);
                            consumer_queue_event(usage_code, 1);
                        } else if (!bit_val && was_pressed) {
                            consumer_track_release(usage_code);
                            consumer_queue_event(usage_code, 0);
                        }

                        report_bit_offset++;
                    }
                } else if (bit_size <= 16 && count == 1) {
                    /* Pattern B: selector value (direct usage code) */
                    int byte_off = report_bit_offset / 8;
                    uint16_t usage_code = 0;

                    if (bit_size <= 8 && byte_off < len) {
                        usage_code = report[byte_off];
                    } else if (bit_size <= 16 && byte_off + 1 < len) {
                        usage_code = (uint16_t)(report[byte_off] |
                                                ((uint32_t)report[byte_off + 1] << 8));
                    }

                    /* For selector reports, usage_code is the consumer usage.
                     * A non-zero value means the key is pressed.
                     * We check if it changed from last report. */
                    int was_pressed = consumer_is_pressed(usage_code);

                    if (usage_code != 0 && !was_pressed) {
                        /* New key pressed */
                        consumer_track_press(usage_code);
                        consumer_queue_event(usage_code, 1);
                    } else if (usage_code == 0) {
                        /* All keys released — release everything */
                        for (int t = 0; t < g_num_tracked; t++) {
                            consumer_queue_event(g_tracked_keys[t], 0);
                        }
                        g_num_tracked = 0;
                    }
                    /* If usage_code changed to a different key,
                     * release old and press new */
                    if (usage_code != 0 && was_pressed) {
                        /* Check if any tracked key is NOT the current one */
                        for (int t = 0; t < g_num_tracked; t++) {
                            if (g_tracked_keys[t] != usage_code) {
                                consumer_queue_event(g_tracked_keys[t], 0);
                                /* Remove old key and re-check */
                                for (int j = t; j < g_num_tracked - 1; j++)
                                    g_tracked_keys[j] = g_tracked_keys[j + 1];
                                g_num_tracked--;
                                t--; /* re-check this index */
                            }
                        }
                    }

                    report_bit_offset += bit_size;
                } else {
                    /* Multi-bit variable or array — skip */
                    report_bit_offset += bit_size * count;
                }
                break;
            }
            case 10: { /* Collection */
                uint8_t coll_type = (uint8_t)(data & 0xFF);
                collection_depth++;
                if (coll_type == HID_COLLECTION_APPLICATION &&
                    usage_page == HID_PAGE_CONSUMER &&
                    cur_usage == HID_USAGE_CONSUMER_CONTROL) {
                    in_consumer = 1;
                }
                break;
            }
            case 12: /* End Collection */
                if (collection_depth > 0) {
                    collection_depth--;
                    if (collection_depth == 0) in_consumer = 0;
                }
                break;
            }

            /* Reset local items after each Main item */
            cur_usage = 0;
            cur_usage_min = 0;
            cur_usage_max = 0;
            cur_report_count = 0;
            break;

        case 1: /* Global items */
            switch (bTag) {
            case 0: usage_page = data; break;
            case 7: cur_report_size = data; break;
            case 8: /* Report ID */ break;
            case 9: cur_report_count = data; break;
            default: break;
            }
            break;

        case 2: /* Local items */
            switch (bTag) {
            case 0: cur_usage = data; break;
            case 1: cur_usage_min = data; break;
            case 2: cur_usage_max = data; break;
            default: break;
            }
            break;
        }

        i += data_bytes;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void usb_hid_consumer_init(void)
{
    if (g_consumer_initialized)
        return;

    memset(&g_consumer, 0, sizeof(g_consumer));
    spinlock_init(&g_consumer.lock);
    g_consumer_initialized = 1;

    kprintf("[CONSUMER] Consumer control subsystem initialised\n");
}

int usb_hid_consumer_register(uint8_t dev_addr, uint8_t intf_num,
                               uint8_t input_ep,
                               const uint8_t *report_desc, int desc_len)
{
    if (!g_consumer_initialized)
        return -EAGAIN;

    struct hid_consumer_dev *dev = &g_consumer;

    if (dev->present) {
        kprintf("[CONSUMER] Consumer device already registered, "
                "replacing\n");
        dev->present = 0;
        if (dev->report_desc) {
            kfree(dev->report_desc);
            dev->report_desc = NULL;
        }
    }

    dev->dev_addr  = dev_addr;
    dev->intf_num  = intf_num;
    dev->input_ep  = input_ep;
    dev->ev_head   = 0;
    dev->ev_tail   = 0;
    g_num_tracked  = 0;

    /* Parse the report descriptor to determine report length */
    int report_len = 0;
    int has_consumer = consumer_parse_report_desc(report_desc, desc_len,
                                                   &report_len);
    if (!has_consumer) {
        kprintf("[CONSUMER] No Consumer Control found in report desc\n");
        return -ENODEV;
    }

    /* Store a copy of the report descriptor */
    if (report_desc && desc_len > 0) {
        dev->report_desc = (uint8_t *)kmalloc((size_t)desc_len);
        if (dev->report_desc) {
            memcpy(dev->report_desc, report_desc, (size_t)desc_len);
            dev->report_desc_len = desc_len;
        }
    }

    dev->report_len = report_len > 0 ? report_len : 4;
    dev->present = 1;

    kprintf("[CONSUMER] Consumer control device registered: "
            "addr=%d, intf=%d, ep=0x%02x, report_len=%d\n",
            dev_addr, intf_num, input_ep, dev->report_len);

    return 0;
}

void usb_hid_consumer_unregister(void)
{
    struct hid_consumer_dev *dev = &g_consumer;

    if (!dev->present)
        return;

    dev->present = 0;

    if (dev->report_desc) {
        kfree(dev->report_desc);
        dev->report_desc = NULL;
    }
    dev->report_desc_len = 0;
    g_num_tracked = 0;

    kprintf("[CONSUMER] Consumer control device unregistered\n");
}

void usb_hid_consumer_input(const uint8_t *report, int len)
{
    struct hid_consumer_dev *dev = &g_consumer;

    if (!dev->present || !report || len <= 0)
        return;

    /* The USB HID core calls this when a new interrupt IN transfer
     * arrives on the consumer endpoint.  Process the report to
     * extract consumer key events. */
    if (len >= dev->report_len) {
        consumer_process_report(report, len);
    }
}

int usb_hid_consumer_present(void)
{
    struct hid_consumer_dev *dev = &g_consumer;
    return dev->present ? 1 : 0;
}

int usb_hid_consumer_get_event(struct hid_consumer_event *out)
{
    struct hid_consumer_dev *dev = &g_consumer;

    if (!out)
        return -EINVAL;

    spinlock_acquire(&dev->lock);

    if (dev->ev_head == dev->ev_tail) {
        spinlock_release(&dev->lock);
        return 0;
    }

    *out = dev->events[dev->ev_head];
    dev->ev_head = (dev->ev_head + 1) % CONSUMER_EVENT_QUEUE;

    spinlock_release(&dev->lock);
    return 1;
}

/*
 * Poll the consumer device interrupt endpoint.
 *
 * This uses the same EHCI MMIO primitives as the main HID driver
 * (ehci_do_transfer / usb_control).  We expect the consumer device
 * to be on its own interrupt endpoint.
 *
 * In practice, many multimedia keyboards multiplex consumer reports
 * on the same endpoint as keyboard reports using Report IDs.  In that
 * case the main HID driver should call usb_hid_consumer_input()
 * directly from its interrupt handler after demuxing by Report ID.
 *
 * For standalone consumer devices (remote controls, audio docks),
 * this function polls the dedicated endpoint.
 */
void usb_hid_consumer_poll(void)
{
    struct hid_consumer_dev *dev = &g_consumer;
    if (!dev->present || !dev->input_ep || !dev->report_len)
        return;

    /* Read the consumer input report via interrupt IN transfer.
     * We reuse the HID driver's transfer primitive by calling the
     * export from usb_hid.c.  For now, we rely on the main HID
     * driver to poll shared endpoints and call consumer_input(). */
    (void)dev;
}

/* ── Callback management ──────────────────────────────────────────── */

void usb_hid_consumer_set_callback(void (*cb)(uint16_t code, int pressed))
{
    g_consumer_callback = cb;
}
