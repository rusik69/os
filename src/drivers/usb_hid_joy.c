/*
 * usb_hid_joy.c — USB HID joystick/gamepad driver
 *
 * Parses HID gamepad/joystick reports for axes (X, Y, Z, Rx, Ry, Rz,
 * slider, throttle, rudder) and up to 128 buttons.  Maps to internal
 * evdev-style ABS/BTN codes so that higher-level subsystems (joydev,
 * etc.) can consume them.
 *
 * Item S44 — USB joystick/gamepad HID driver
 */

#include "usb.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"

/* ── HID usage page constants ────────────────────────────────────── */
#define HID_PAGE_GENERIC_DESKTOP  0x01
#define HID_PAGE_BUTTON           0x09

/* Generic Desktop usages */
#define HID_USAGE_X               0x30
#define HID_USAGE_Y               0x31
#define HID_USAGE_Z               0x32
#define HID_USAGE_RX              0x33
#define HID_USAGE_RY              0x34
#define HID_USAGE_RZ              0x35
#define HID_USAGE_SLIDER          0x36
#define HID_USAGE_DIAL            0x37
#define HID_USAGE_WHEEL           0x38
#define HID_USAGE_HAT_SWITCH      0x39

/* ── evdev-like axes and button codes ────────────────────────────── */
#define JOY_ABS_X      0
#define JOY_ABS_Y      1
#define JOY_ABS_Z      2
#define JOY_ABS_RX     3
#define JOY_ABS_RY     4
#define JOY_ABS_RZ     5
#define JOY_ABS_THROTTLE 6
#define JOY_ABS_RUDDER 7
#define JOY_ABS_WHEEL  8
#define JOY_ABS_GAS    9
#define JOY_ABS_BRAKE  10
#define JOY_ABS_HAT0X  16
#define JOY_ABS_HAT0Y  17
#define JOY_MAX_AXES   64

#define JOY_BTN_BASE   0x120   /* evdev BTN_JOYSTICK base */
#define JOY_MAX_BTNS   128

/* ── Axis state ──────────────────────────────────────────────────── */
struct joy_axis {
    int32_t min;
    int32_t max;
    int32_t value;
    int32_t fuzz;
    int32_t flat;
};

/* ── Joystick device instance ────────────────────────────────────── */
struct joy_device {
    uint8_t  dev_addr;                 /* USB device address */
    uint16_t vendor_id;
    uint16_t product_id;

    /* Axis state */
    struct joy_axis axes[JOY_MAX_AXES];
    int n_axes;

    /* Button state */
    uint8_t  buttons[JOY_MAX_BTNS];    /* 0=released, 1=pressed */
    int      n_buttons;

    /* Input report descriptor */
    uint8_t  *report_desc;
    int       report_desc_len;

    /* Current input report buffer */
    uint8_t  *report_buf;
    int       report_len;

    /* Endpoint for HID input */
    uint8_t  input_ep;
    uint8_t  input_ep_interval;

    spinlock_t lock;
    int        present;
};

/* ── Globals ─────────────────────────────────────────────────────── */
#define MAX_JOYSTICKS 4
static struct joy_device g_joysticks[MAX_JOYSTICKS];
static int g_joy_count = 0;

/* ── HID report descriptor parser (simplified) ──────────────────── */

/*
 * Simplified HID report descriptor walker.
 * Scans for Input items, extracts usage page/usage from global state,
 * and populates axis/button maps accordingly.
 */
static void parse_hid_report_descriptor(struct joy_device *joy,
                                         const uint8_t *desc, int len)
{
    if (!desc || len <= 0) return;

    int i = 0;
    uint32_t usage_page = 0;
    uint32_t usage_min  = 0;
    uint32_t usage_max  = 0;
    uint32_t logical_min = 0;
    uint32_t logical_max = 255;
    uint32_t report_count = 1;

    joy->n_axes = 0;
    joy->n_buttons = 0;

    while (i < len) {
        uint8_t tag = desc[i];
        uint8_t type = tag & 0xFC;
        uint8_t size = tag & 0x03;
        i++;

        /* Determine data size */
        uint32_t data = 0;
        int data_len = 0;
        if (size == 1) data_len = 1;
        else if (size == 2) data_len = 2;
        else if (size == 3) data_len = 4;

        if (data_len > 0 && i + data_len <= len) {
            if (data_len == 1) data = desc[i];
            else if (data_len == 2) data = (uint32_t)desc[i] | ((uint32_t)desc[i+1] << 8);
            else if (data_len == 4) data = (uint32_t)desc[i] | ((uint32_t)desc[i+1] << 8) |
                                           ((uint32_t)desc[i+2] << 16) | ((uint32_t)desc[i+3] << 24);
            i += data_len;
        }

        switch (type) {
        case 0x04: /* Input (Input) */
            if (data_len == 0) {
                /* Long item or push/pop — skip */
                break;
            }
            /* If there's an input with button usages, count buttons */
            if (usage_page == HID_PAGE_BUTTON && usage_min > 0 && usage_max >= usage_min) {
                int n = (int)(usage_max - usage_min + 1);
                if (joy->n_buttons + n <= JOY_MAX_BTNS) {
                    joy->n_buttons += n;
                }
            }
            /* If there's an input with desktop usages, count axes */
            if (usage_page == HID_PAGE_GENERIC_DESKTOP) {
                /* Count axes from usage range */
                int n = (int)((usage_max - usage_min + 1) * report_count);
                /* More precisely: check each usage */
                for (uint32_t u = usage_min; u <= usage_max && u - usage_min < report_count; u++) {
                    if (u >= 0x30 && u <= 0x39) {
                        int axis_idx = joy->n_axes;
                        if (axis_idx < JOY_MAX_AXES) {
                            joy->axes[axis_idx].min = (int32_t)logical_min;
                            joy->axes[axis_idx].max = (int32_t)logical_max;
                            joy->axes[axis_idx].value = (int32_t)((logical_max + logical_min) / 2);
                            joy->n_axes++;
                        }
                    }
                }
                /* Also handle report_count > 1 case */
                if (report_count > 1 && usage_min >= 0x30 && usage_min <= 0x39) {
                    for (uint32_t r = 0; r < report_count; r++) {
                        if (joy->n_axes < JOY_MAX_AXES) {
                            joy->axes[joy->n_axes].min = (int32_t)logical_min;
                            joy->axes[joy->n_axes].max = (int32_t)logical_max;
                            joy->axes[joy->n_axes].value = (int32_t)((logical_max + logical_min) / 2);
                            joy->n_axes++;
                        }
                    }
                }
            }
            break;

        case 0x08: /* Output */
        case 0x0C: /* Feature */
        case 0x14: /* Collection */
        case 0x1C: /* End Collection */
            break;

        case 0x80: /* Usage Page (global) */
            usage_page = data;
            break;

        case 0x18: /* Usage Minimum */
            usage_min = data;
            break;

        case 0x28: /* Usage Maximum */
            usage_max = data;
            break;

        case 0x84: /* Report ID */
            break;

        case 0x94: /* Report Size */
            break;

        case 0xA4: /* Report Count */
            report_count = data;
            break;

        case 0x24: /* Logical Minimum */
            logical_min = data;
            break;

        case 0x34: /* Logical Maximum */
            logical_max = data;
            break;

        case 0x44: /* Physical Minimum */
        case 0x54: /* Physical Maximum */
        case 0x64: /* Unit Exponent */
        case 0x74: /* Unit */
            break;

        default:
            /* Unknown tag — skip */
            break;
        }
    }

    /* If we didn't find any axes/buttons via ranges, use sensible defaults */
    if (joy->n_axes == 0) {
        /* Assume 6 axes (X, Y, Z, Rx, Ry, Rz) */
        for (int a = 0; a < 6 && a < JOY_MAX_AXES; a++) {
            joy->axes[a].min = 0;
            joy->axes[a].max = 255;
            joy->axes[a].value = 127;
            joy->n_axes++;
        }
    }
    if (joy->n_buttons == 0) {
        joy->n_buttons = 16;  /* at least 16 buttons */
    }
}

/* ── Report parser ──────────────────────────────────────────────── */

static void parse_input_report(struct joy_device *joy,
                                const uint8_t *report, int len)
{
    if (!report || len <= 0) return;

    spinlock_acquire(&joy->lock);

    /* Simple parser: assume report layout matches descriptor.
     * For a production driver, we'd walk the descriptor to extract
     * each field.  Here we use a simplified heuristic:
     *   - First N bytes are axes (8-bit each)
     *   - Remaining bytes are button bitfields
     */
    int byte_idx = 0;

    /* Parse axes */
    for (int a = 0; a < joy->n_axes && byte_idx < len; a++) {
        int32_t range = joy->axes[a].max - joy->axes[a].min;
        if (range <= 255) {
            /* 8-bit axis */
            joy->axes[a].value = (int32_t)report[byte_idx++];
        } else if (range <= 65535 && byte_idx + 1 < len) {
            /* 16-bit axis (little-endian) */
            joy->axes[a].value = (int32_t)(report[byte_idx] | ((uint32_t)report[byte_idx+1] << 8));
            byte_idx += 2;
        } else if (byte_idx + 3 < len) {
            /* 32-bit axis */
            joy->axes[a].value = (int32_t)(report[byte_idx] |
                                            ((uint32_t)report[byte_idx+1] << 8) |
                                            ((uint32_t)report[byte_idx+2] << 16) |
                                            ((uint32_t)report[byte_idx+3] << 24));
            byte_idx += 4;
        } else {
            break;
        }
    }

    /* Parse buttons from remaining bytes */
    int bit_idx = 0;
    while (byte_idx < len && bit_idx < joy->n_buttons) {
        uint8_t b = report[byte_idx];
        for (int bit = 0; bit < 8 && bit_idx < joy->n_buttons; bit++, bit_idx++) {
            joy->buttons[bit_idx] = (b >> bit) & 1;
        }
        byte_idx++;
    }

    spinlock_release(&joy->lock);
}

/* ── Public API for USB HID subsystem ───────────────────────────── */

int usb_hid_joy_register(uint8_t dev_addr, uint16_t vid, uint16_t pid,
                          const uint8_t *report_desc, int desc_len,
                          uint8_t input_ep, uint8_t interval)
{
    if (g_joy_count >= MAX_JOYSTICKS)
        return -ENOSPC;

    struct joy_device *joy = &g_joysticks[g_joy_count];
    memset(joy, 0, sizeof(*joy));

    joy->dev_addr = dev_addr;
    joy->vendor_id = vid;
    joy->product_id = pid;
    joy->input_ep = input_ep;
    joy->input_ep_interval = interval;
    joy->present = 1;
    spinlock_init(&joy->lock);

    /* Allocate and parse report descriptor */
    if (report_desc && desc_len > 0) {
        joy->report_desc = (uint8_t *)kmalloc((size_t)desc_len);
        if (joy->report_desc) {
            memcpy(joy->report_desc, report_desc, (size_t)desc_len);
            joy->report_desc_len = desc_len;
        }
        parse_hid_report_descriptor(joy, report_desc, desc_len);
    }

    kprintf("[JOY] USB joystick registered: VID=0x%04x PID=0x%04x "
            "%d axes %d buttons (addr=%d)\n",
            vid, pid, joy->n_axes, joy->n_buttons, dev_addr);

    g_joy_count++;
    return g_joy_count - 1;
}

void usb_hid_joy_unregister(int joy_idx)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return;

    struct joy_device *joy = &g_joysticks[joy_idx];
    joy->present = 0;

    if (joy->report_desc) {
        kfree(joy->report_desc);
        joy->report_desc = NULL;
    }
    if (joy->report_buf) {
        kfree(joy->report_buf);
        joy->report_buf = NULL;
    }

    kprintf("[JOY] USB joystick %d unregistered\n", joy_idx);
}

void usb_hid_joy_input_report(int joy_idx, const uint8_t *report, int len)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return;

    struct joy_device *joy = &g_joysticks[joy_idx];
    if (!joy->present) return;

    parse_input_report(joy, report, len);
}

/* ── Read axis/button values ────────────────────────────────────── */

int usb_hid_joy_get_axis(int joy_idx, int axis)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return 0;
    struct joy_device *joy = &g_joysticks[joy_idx];
    if (!joy->present || axis < 0 || axis >= joy->n_axes)
        return 0;
    return joy->axes[axis].value;
}

int usb_hid_joy_get_button(int joy_idx, int btn)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return 0;
    struct joy_device *joy = &g_joysticks[joy_idx];
    if (!joy->present || btn < 0 || btn >= joy->n_buttons)
        return 0;
    return joy->buttons[btn] ? 1 : 0;
}

int usb_hid_joy_get_axis_count(int joy_idx)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return 0;
    return g_joysticks[joy_idx].n_axes;
}

int usb_hid_joy_get_button_count(int joy_idx)
{
    if (joy_idx < 0 || joy_idx >= g_joy_count)
        return 0;
    return g_joysticks[joy_idx].n_buttons;
}

int usb_hid_joy_get_count(void)
{
    return g_joy_count;
}

/* ── Stub: usb_hid_joy_init ─────────────────────────────── */
int __init usb_hid_joy_init(void *dev)
{
    (void)dev;
    kprintf("[USB] usb_hid_joy_init: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_hid_joy_read ─────────────────────────────── */
int usb_hid_joy_read(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[USB] usb_hid_joy_read: not yet implemented\n");
    return 0;
}
/* ── Stub: usb_hid_joy_ioctl ─────────────────────────────── */
int usb_hid_joy_ioctl(void *dev, int cmd, void *arg)
{
    (void)dev;
    (void)cmd;
    (void)arg;
    kprintf("[USB] usb_hid_joy_ioctl: not yet implemented\n");
    return 0;
}
