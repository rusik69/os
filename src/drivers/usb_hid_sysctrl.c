/*
 * usb_hid_sysctrl.c — USB HID System Control driver (power, sleep, wake)
 *
 * Implements HID System Control support for power, sleep, wake, and
 * other system control keys defined in the Generic Desktop page (0x01)
 * at usages 0x80–0x8F.
 *
 * The driver parses HID report descriptors to detect System Control
 * application collections, then processes raw input reports to generate
 * press/release events for system control keys.
 *
 * References:
 *   USB HID Usage Tables, §4 — Generic Desktop Page (0x01)
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

/* ── System control key state tracking ─────────────────────────────── */

/*
 * Maximum number of distinct system control usages we track simultaneously.
 * System control reports typically have 3 keys (power, sleep, wake).
 */
#define SYSCTRL_MAX_TRACKED    16

/*
 * Tracked system control key state — remembers which keys are currently held.
 */
static uint16_t g_sysctrl_tracked[SYSCTRL_MAX_TRACKED];
static int      g_sysctrl_num_tracked = 0;

/* Global system control device state */
static struct hid_sysctrl_dev g_sysctrl;
static int g_sysctrl_initialized = 0;

/* Optional callback for system control key events */
static void (*g_sysctrl_callback)(uint16_t code, int pressed) = NULL;

/* ── Internal helpers ─────────────────────────────────────────────── */

/*
 * Check if a system control usage is currently tracked as pressed.
 */
static int sysctrl_is_pressed(uint16_t code)
{
    for (int i = 0; i < g_sysctrl_num_tracked; i++) {
        if (g_sysctrl_tracked[i] == code)
            return 1;
    }
    return 0;
}

/*
 * Add a system control usage to the tracked-pressed set.
 */
static void sysctrl_track_press(uint16_t code)
{
    if (g_sysctrl_num_tracked >= SYSCTRL_MAX_TRACKED)
        return;
    /* Don't add duplicates */
    if (sysctrl_is_pressed(code))
        return;
    g_sysctrl_tracked[g_sysctrl_num_tracked++] = code;
}

/*
 * Remove a system control usage from the tracked-pressed set.
 */
static void sysctrl_track_release(uint16_t code)
{
    for (int i = 0; i < g_sysctrl_num_tracked; i++) {
        if (g_sysctrl_tracked[i] == code) {
            /* Shift remaining entries down */
            for (int j = i; j < g_sysctrl_num_tracked - 1; j++)
                g_sysctrl_tracked[j] = g_sysctrl_tracked[j + 1];
            g_sysctrl_num_tracked--;
            return;
        }
    }
}

/*
 * Queue a system control event in the ring buffer.
 */
static void sysctrl_queue_event(uint16_t code, int pressed)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;

    spinlock_acquire(&dev->lock);

    int next = (dev->ev_tail + 1) % SYSCTRL_EVENT_QUEUE;
    if (next != dev->ev_head) {
        dev->events[dev->ev_tail].code    = code;
        dev->events[dev->ev_tail].pressed = pressed;
        dev->ev_tail = next;
    }

    spinlock_release(&dev->lock);

    /* Fire optional callback */
    if (g_sysctrl_callback)
        g_sysctrl_callback(code, pressed);
}

/* ── HID report descriptor parser (system control-specific) ────────── */

/*
 * Parse the HID report descriptor to extract system control usage info.
 *
 * Scans for:
 *  - Collection(Application, System Control) — marks the device for
 *    system controls
 *  - Input items within a System Control collection — tracks report size
 *  - Usage items to understand the system control usages being reported
 *
 * Returns 1 if system control usages are found, 0 otherwise.
 * The expected report length is written to @out_report_len.
 */
static int sysctrl_parse_report_desc(const uint8_t *desc, int len,
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
    int in_sysctrl = 0;
    int collection_depth = 0;
    int sysctrl_collections = 0;
    int total_bits = 0;
    int found_sysctrl = 0;

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
                if (in_sysctrl && report_count > 0 && report_size > 0) {
                    total_bits += (int)(report_count * report_size);
                    found_sysctrl = 1;
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

                /* Check if this is a System Control Application Collection */
                if (coll_type == HID_COLLECTION_APPLICATION &&
                    usage_page == HID_PAGE_GENERIC_DESKTOP &&
                    usage == HID_USAGE_SYSTEM_CONTROL) {
                    in_sysctrl = 1;
                    sysctrl_collections++;
                    kprintf("[SYSCTRL] Found System Control collection\\n");
                }
                break;
            }

            case 12: /* End Collection */
                if (collection_depth > 0) {
                    collection_depth--;
                    if (collection_depth == 0)
                        in_sysctrl = 0;
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

    if (found_sysctrl && total_bits > 0) {
        *out_report_len = (total_bits + 7) / 8;
        kprintf("[SYSCTRL] Report length: %d bytes (%d bits)\\n",
                *out_report_len, total_bits);
        return 1;
    }

    /* Even without finding Input items, if we saw a System Control
     * collection, assume a default 2-byte report. */
    if (sysctrl_collections > 0) {
        *out_report_len = 2;
        return 1;
    }

    return 0;
}

/* ── Input report parser ──────────────────────────────────────────── */

/*
 * Extract system control usages from a raw HID input report.
 *
 * System control HID reports typically use a bitmap of 1-bit toggles
 * (same pattern as consumer controls):
 *
 * Pattern A: 1-bit toggle array (e.g., report_count=3, report_size=1)
 *   Usage Minimum = System Power Down (0x81)
 *   Usage Maximum = System Wake Up (0x83)
 *   Each bit position maps to a usage in [min, max] range.
 *
 * Pattern B: 8/16-bit selector (e.g., report_count=1, report_size=8)
 *   The report value directly encodes a HID_USAGE_SYSTEM_* code.
 *   Non-zero means pressed.
 */
static void sysctrl_process_report(const uint8_t *report, int len)
{
    if (!report || len <= 0) return;

    struct hid_sysctrl_dev *dev = &g_sysctrl;
    if (!dev->report_desc || dev->report_desc_len <= 0)
        return;

    const uint8_t *desc = dev->report_desc;
    int dlen = dev->report_desc_len;

    /* Walk the descriptor to find Input items within System Control */
    int i = 0;
    uint32_t usage_page = 0;
    uint32_t cur_usage = 0;
    uint32_t cur_usage_min = 0;
    uint32_t cur_usage_max = 0;
    uint32_t cur_report_size = 8;
    uint32_t cur_report_count = 0;
    int in_sysctrl = 0;
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
                if (!in_sysctrl || cur_report_count == 0)
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

                if (is_variable && bit_size == 1 &&
                    cur_usage_max >= cur_usage_min) {
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

                        int was_pressed = sysctrl_is_pressed(usage_code);
                        if (bit_val && !was_pressed) {
                            sysctrl_track_press(usage_code);
                            sysctrl_queue_event(usage_code, 1);
                        } else if (!bit_val && was_pressed) {
                            sysctrl_track_release(usage_code);
                            sysctrl_queue_event(usage_code, 0);
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

                    int was_pressed = sysctrl_is_pressed(usage_code);

                    if (usage_code != 0 && !was_pressed) {
                        sysctrl_track_press(usage_code);
                        sysctrl_queue_event(usage_code, 1);
                    } else if (usage_code == 0) {
                        /* All keys released — release everything */
                        for (int t = 0; t < g_sysctrl_num_tracked; t++) {
                            sysctrl_queue_event(g_sysctrl_tracked[t], 0);
                        }
                        g_sysctrl_num_tracked = 0;
                    }
                    /* If usage_code changed to a different key */
                    if (usage_code != 0 && was_pressed) {
                        for (int t = 0; t < g_sysctrl_num_tracked; t++) {
                            if (g_sysctrl_tracked[t] != usage_code) {
                                sysctrl_queue_event(g_sysctrl_tracked[t], 0);
                                for (int j = t; j < g_sysctrl_num_tracked - 1; j++)
                                    g_sysctrl_tracked[j] = g_sysctrl_tracked[j + 1];
                                g_sysctrl_num_tracked--;
                                t--;
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
                    usage_page == HID_PAGE_GENERIC_DESKTOP &&
                    cur_usage == HID_USAGE_SYSTEM_CONTROL) {
                    in_sysctrl = 1;
                }
                break;
            }
            case 12: /* End Collection */
                if (collection_depth > 0) {
                    collection_depth--;
                    if (collection_depth == 0) in_sysctrl = 0;
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

/* ── Public API ────────────────────────────────────────────────────── */

void usb_hid_sysctrl_init(void)
{
    if (g_sysctrl_initialized)
        return;

    memset(&g_sysctrl, 0, sizeof(g_sysctrl));
    spinlock_init(&g_sysctrl.lock);
    g_sysctrl_initialized = 1;

    kprintf("[SYSCTRL] System Control subsystem initialised\n");
}

int usb_hid_sysctrl_register(uint8_t dev_addr, uint8_t intf_num,
                              uint8_t input_ep,
                              const uint8_t *report_desc, int desc_len)
{
    if (!g_sysctrl_initialized)
        return -EAGAIN;

    struct hid_sysctrl_dev *dev = &g_sysctrl;

    if (dev->present) {
        kprintf("[SYSCTRL] System Control device already registered, "
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
    g_sysctrl_num_tracked = 0;

    /* Parse the report descriptor to determine report length */
    int report_len = 0;
    int has_sysctrl = sysctrl_parse_report_desc(report_desc, desc_len,
                                                 &report_len);
    if (!has_sysctrl) {
        kprintf("[SYSCTRL] No System Control found in report desc\n");
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

    dev->report_len = report_len > 0 ? report_len : 2;
    dev->present = 1;

    kprintf("[SYSCTRL] System Control device registered: "
            "addr=%d, intf=%d, ep=0x%02x, report_len=%d\n",
            dev_addr, intf_num, input_ep, dev->report_len);

    return 0;
}

void usb_hid_sysctrl_unregister(void)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;

    if (!dev->present)
        return;

    dev->present = 0;

    if (dev->report_desc) {
        kfree(dev->report_desc);
        dev->report_desc = NULL;
    }
    dev->report_desc_len = 0;
    g_sysctrl_num_tracked = 0;

    kprintf("[SYSCTRL] System Control device unregistered\n");
}

void usb_hid_sysctrl_input(const uint8_t *report, int len)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;

    if (!dev->present || !report || len <= 0)
        return;

    if (len >= dev->report_len) {
        sysctrl_process_report(report, len);
    }
}

int usb_hid_sysctrl_present(void)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;
    return dev->present ? 1 : 0;
}

int usb_hid_sysctrl_get_event(struct hid_sysctrl_event *out)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;

    if (!out)
        return -EINVAL;

    spinlock_acquire(&dev->lock);

    if (dev->ev_head == dev->ev_tail) {
        spinlock_release(&dev->lock);
        return 0;
    }

    *out = dev->events[dev->ev_head];
    dev->ev_head = (dev->ev_head + 1) % SYSCTRL_EVENT_QUEUE;

    spinlock_release(&dev->lock);
    return 1;
}

/*
 * Poll the system control device interrupt endpoint.
 *
 * Follows the same pattern as usb_hid_consumer_poll: system control
 * reports are typically multiplexed on the same interrupt endpoint as
 * keyboard reports using Report IDs.  In that case the main HID driver
 * calls usb_hid_sysctrl_input() directly from its interrupt handler.
 *
 * For standalone system control devices, this function polls the
 * dedicated endpoint.
 */
void usb_hid_sysctrl_poll(void)
{
    struct hid_sysctrl_dev *dev = &g_sysctrl;
    if (!dev->present || !dev->input_ep || !dev->report_len)
        return;

    /* For now, rely on the main HID driver to call sysctrl_input()
     * from the shared interrupt handler after demuxing by Report ID.
     * This function is provided for future standalone devices. */
    (void)dev;
}

/* ── Callback management ───────────────────────────────────────────── */

void usb_hid_sysctrl_set_callback(void (*cb)(uint16_t code, int pressed))
{
    g_sysctrl_callback = cb;
}
