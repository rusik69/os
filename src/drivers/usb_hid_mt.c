/*
 * usb_hid_mt.c — USB HID Multi-Touch Protocol driver
 *
 * Implements HID multi-touch digitizer support for touchscreens and
 * touchpads using the USB HID protocol.  Handles serial mode (one
 * contact per report) with Contact ID tracking, and provides an API
 * for higher-level subsystems to consume touch events.
 *
 * References:
 *   USB HID Usage Tables, §16 — Digitizer Page (0x0D)
 *   Windows Precision Touchpad Protocol (PTP)
 *   HID Over I²C Protocol Specification, §5.1 — Multi-touch
 *
 * Item S44 — USB HID multi-touch driver
 */

#include "uhid.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

/* ── Bit-level helpers ──────────────────────────────────────────────── */

/*
 * Extract an unsigned value from a bit-field within a byte buffer.
 */
static uint32_t mt_extract_bits(const uint8_t *buf, int bit_off, int bit_size)
{
    uint32_t val = 0;
    int i;

    if (!buf || bit_size <= 0 || bit_size > 32)
        return 0;

    for (i = 0; i < bit_size; i++) {
        int byte_idx = (bit_off + i) >> 3;
        int bit_idx  = (bit_off + i) & 7;
        val |= ((uint32_t)((buf[byte_idx] >> bit_idx) & 1)) << i;
    }
    return val;
}

/*
 * Read a value from the report at the given byte offset.
 */
static int32_t mt_read_value(const uint8_t *buf, int off, int size_bytes)
{
    if (!buf || off < 0 || size_bytes <= 0)
        return 0;

    switch (size_bytes) {
    case 1:
        return (int32_t)(int8_t)buf[off];
    case 2:
        return (int32_t)(int16_t)((uint32_t)buf[off] |
                                   ((uint32_t)buf[off + 1] << 8));
    case 4:
        return (int32_t)((uint32_t)buf[off] |
                          ((uint32_t)buf[off + 1] << 8) |
                          ((uint32_t)buf[off + 2] << 16) |
                          ((uint32_t)buf[off + 3] << 24));
    default:
        return 0;
    }
}

/* ── Field location recorder ────────────────────────────────────────── */

/*
 * Record a single field location into the mt_device.
 * Converts from bit offset to byte offset for simple fields,
 * and stores bit-level info for sub-byte fields (Tip Switch, Confidence).
 */
static void mt_record_field(struct mt_device *mt,
                             uint32_t usage_page, uint32_t usage_id,
                             int bit_offset, int bit_size)
{
    int byte_offset = bit_offset / 8;
    int bit_in_byte = bit_offset % 8;
    uint8_t mask = (uint8_t)(((1u << bit_size) - 1) << bit_in_byte);

    if (usage_page == HID_PAGE_DIGITIZER) {
        switch (usage_id) {
        case HID_USAGE_CONTACT_ID:
            if (mt->off_contact_id < 0) {
                mt->off_contact_id = byte_offset;
                mt->size_contact_id = bit_size;
            }
            break;
        case HID_USAGE_CONTACT_COUNT:
            if (mt->off_contact_count < 0)
                mt->off_contact_count = byte_offset;
            break;
        case HID_USAGE_TIP_SWITCH:
            if (mt->off_tip_switch < 0 && bit_size <= 8) {
                mt->off_tip_switch = byte_offset;
                mt->mask_tip_switch = mask;
            }
            break;
        case HID_USAGE_CONFIDENCE:
            if (mt->off_confidence < 0 && bit_size <= 8) {
                mt->off_confidence = byte_offset;
                mt->mask_confidence = mask;
            }
            break;
        case HID_USAGE_TIP_PRESSURE:
            if (mt->off_pressure < 0 || mt->off_pressure == -1)
                mt->off_pressure = byte_offset;
            break;
        case HID_USAGE_WIDTH:
            if (mt->off_width < 0 || mt->off_width == -1)
                mt->off_width = byte_offset;
            break;
        case HID_USAGE_HEIGHT:
            if (mt->off_height < 0 || mt->off_height == -1)
                mt->off_height = byte_offset;
            break;
        case HID_USAGE_SCAN_TIME:
            if (mt->off_scan_time < 0)
                mt->off_scan_time = byte_offset;
            break;
        default:
            break;
        }
    }

    if (usage_page == HID_PAGE_GENERIC_DESKTOP) {
        switch (usage_id) {
        case HID_USAGE_X:
            if (mt->off_x < 0) {
                mt->off_x = byte_offset;
                mt->size_x = bit_size;
            }
            break;
        case HID_USAGE_Y:
            if (mt->off_y < 0) {
                mt->off_y = byte_offset;
                mt->size_y = bit_size;
            }
            break;
        default:
            break;
        }
    }
}

/* ── Field offset computation ───────────────────────────────────────── */

/*
 * Walk the parsed HID report descriptor items, tracking bit positions,
 * and record the byte/bit offset of each multi-touch-relevant field
 * into the mt_device structure.
 */
static void mt_compute_offsets(struct mt_device *mt,
                                const struct hid_report_desc *desc)
{
    int i;
    int bit_pos = 0;

    /* Initialise all offsets to "not found" */
    mt->off_contact_count = -1;
    mt->off_contact_id    = -1;
    mt->off_x             = -1;
    mt->off_y             = -1;
    mt->off_width         = -1;
    mt->off_height        = -1;
    mt->off_pressure      = -1;
    mt->off_scan_time     = -1;
    mt->off_tip_switch    = -1;
    mt->mask_tip_switch   = 0;
    mt->off_confidence    = -1;
    mt->mask_confidence   = 0;
    mt->size_x            = 0;
    mt->size_y            = 0;
    mt->size_contact_id   = 0;
    mt->per_contact_bits  = 0;

    /* Track collection nesting to find per-contact field boundaries */
    int coll_depth = 0;
    int contact_set_start_bit = -1;
    int in_mt_app = 0;
    int in_contact_coll = 0;

    for (i = 0; i < desc->num_items; i++) {
        const struct hid_report_item *ri = &desc->items[i];
        uint32_t us = ri->local.usage;
        uint32_t up = ri->global.usage_page;
        int item_bits = (int)(ri->global.report_size *
                              ri->global.report_count);

        switch (ri->tag) {
        case HID_ITEM_COLLECTION: {
            coll_depth++;
            if (coll_depth == 1) {
                if (up == HID_PAGE_DIGITIZER &&
                    (us == HID_USAGE_TOUCH_SCREEN ||
                     us == HID_USAGE_TOUCH_PAD)) {
                    in_mt_app = 1;
                }
            }
            if (in_mt_app && coll_depth == 2) {
                in_contact_coll = 1;
                if (contact_set_start_bit < 0)
                    contact_set_start_bit = bit_pos;
            }
            break;
        }

        case HID_ITEM_END_COLLECTION: {
            if (in_contact_coll && coll_depth == 2)
                in_contact_coll = 0;
            if (in_mt_app && coll_depth == 1)
                in_mt_app = 0;
            if (coll_depth > 0)
                coll_depth--;
            break;
        }

        case HID_ITEM_INPUT: {
            if (!(ri->flags & HID_IOF_CONST) &&
                (ri->flags & HID_IOF_VARIABLE)) {
                uint32_t usage_min = ri->local.usage_minimum;
                uint32_t usage_max = ri->local.usage_maximum;
                int rc = (int)ri->global.report_count;
                int rs = (int)ri->global.report_size;
                uint32_t single_usage = ri->local.usage;

                /* Determine the usages for each field */
                if (usage_min > 0 && usage_max >= usage_min &&
                    (int)(usage_max - usage_min + 1) >= rc) {
                    int f;
                    for (f = 0; f < rc; f++) {
                        uint32_t u = usage_min +
                            (uint32_t)(f % (int)(usage_max - usage_min + 1));
                        mt_record_field(mt, up, u,
                                        bit_pos + f * rs, rs);
                    }
                } else if (single_usage > 0) {
                    int f;
                    for (f = 0; f < rc; f++) {
                        mt_record_field(mt, up, single_usage,
                                        bit_pos + f * rs, rs);
                    }
                }
            }

            bit_pos += item_bits;
            break;
        }

        case HID_ITEM_OUTPUT:
        case HID_ITEM_FEATURE:
            bit_pos += item_bits;
            break;

        default:
            break;
        }
    }

    /* Compute per-contact data size */
    if (contact_set_start_bit >= 0 && mt->off_contact_id >= 0) {
        mt->per_contact_bits = bit_pos - contact_set_start_bit;
        if (mt->per_contact_bits <= 0)
            mt->per_contact_bits = bit_pos;
    }

    kprintf("[usb_hid_mt] Field offsets: x@%d y@%d id@%d tip@%d "
            "per_contact=%d bits\n",
            mt->off_x, mt->off_y, mt->off_contact_id,
            mt->off_tip_switch, mt->per_contact_bits);
}

/* ── Contact slot management ────────────────────────────────────────── */

static int mt_find_slot(struct mt_device *mt, uint8_t contact_id)
{
    int i;
    for (i = 0; i < MT_MAX_CONTACTS; i++) {
        if (mt->contacts[i].active &&
            mt->contacts[i].contact_id == contact_id) {
            return i;
        }
    }
    return -1;
}

static int mt_find_free_slot(struct mt_device *mt)
{
    int i;
    for (i = 0; i < MT_MAX_CONTACTS; i++) {
        if (!mt->contacts[i].active)
            return i;
    }
    return -1;
}

/* ── Report processing ──────────────────────────────────────────────── */

void usb_hid_mt_process_report(struct mt_device *mt,
                                const uint8_t *report, int len)
{
    int contact_id = 0;
    int tip = 0;
    int confidence = 0;
    int32_t x = 0, y = 0, pressure = 0;
    int32_t width = 0, height = 0;

    if (!mt || !report || len <= 0 || !mt->present)
        return;

    spinlock_acquire(&mt->lock);

    /* Extract Contact Count (if available) */
    if (mt->off_contact_count >= 0 &&
        mt->off_contact_count < len) {
        mt->contact_count = (int)report[mt->off_contact_count];
    }

    /* Extract Contact ID */
    if (mt->off_contact_id >= 0 && mt->off_contact_id < len &&
        mt->size_contact_id > 0 && mt->size_contact_id <= 8) {
        contact_id = (int)mt_extract_bits(report,
            mt->off_contact_id * 8, mt->size_contact_id);
    }

    /* Extract X coordinate */
    if (mt->off_x >= 0 && mt->off_x < len && mt->size_x > 0) {
        int x_bytes = (mt->size_x + 7) / 8;
        if (mt->off_x + x_bytes <= len) {
            if (mt->size_x <= 8)
                x = (int32_t)(int8_t)report[mt->off_x];
            else
                x = mt_read_value(report, mt->off_x,
                                  x_bytes <= 4 ? x_bytes : 2);
        }
    }

    /* Extract Y coordinate */
    if (mt->off_y >= 0 && mt->off_y < len && mt->size_y > 0) {
        int y_bytes = (mt->size_y + 7) / 8;
        if (mt->off_y + y_bytes <= len) {
            if (mt->size_y <= 8)
                y = (int32_t)(int8_t)report[mt->off_y];
            else
                y = mt_read_value(report, mt->off_y,
                                  y_bytes <= 4 ? y_bytes : 2);
        }
    }

    /* Extract Tip Switch */
    if (mt->off_tip_switch >= 0 && mt->off_tip_switch < len &&
        mt->mask_tip_switch) {
        tip = (report[mt->off_tip_switch] & mt->mask_tip_switch) ? 1 : 0;
    }

    /* Extract Confidence */
    if (mt->off_confidence >= 0 && mt->off_confidence < len &&
        mt->mask_confidence) {
        confidence = (report[mt->off_confidence] & mt->mask_confidence) ? 1 : 0;
    }

    /* Extract Pressure */
    if (mt->off_pressure >= 0 && mt->off_pressure < len) {
        pressure = (int32_t)report[mt->off_pressure];
    }

    /* Extract Width */
    if (mt->off_width >= 0 && mt->off_width < len) {
        width = (int32_t)report[mt->off_width];
    }

    /* Extract Height */
    if (mt->off_height >= 0 && mt->off_height < len) {
        height = (int32_t)report[mt->off_height];
    }

    /* ── Update contact tracking ──────────────────────────────────── */

    if (tip) {
        /* Contact is touching — find or allocate a slot */
        int slot = mt_find_slot(mt, (uint8_t)contact_id);
        if (slot < 0) {
            slot = mt_find_free_slot(mt);
            if (slot < 0) {
                /* All slots full — evict the first one */
                slot = 0;
            }
        }

        struct mt_contact *c = &mt->contacts[slot];
        if (!c->active) {
            c->active = 1;
            mt->num_contacts++;
        }

        c->contact_id = (uint8_t)contact_id;
        c->tip        = tip;
        c->in_range   = 1;
        c->confidence = confidence;
        c->x          = x;
        c->y          = y;
        c->pressure   = pressure;
        c->width      = width;
        c->height     = height;
    } else {
        /* Contact released — remove it */
        int slot = mt_find_slot(mt, (uint8_t)contact_id);
        if (slot >= 0) {
            struct mt_contact *c = &mt->contacts[slot];
            memset(c, 0, sizeof(*c));
            mt->num_contacts--;
            if (mt->num_contacts < 0)
                mt->num_contacts = 0;
        }
    }

    spinlock_release(&mt->lock);
}

/* ── Public API ─────────────────────────────────────────────────────── */

int usb_hid_mt_init(struct mt_device *mt, uint8_t dev_addr,
                     const struct hid_report_desc *desc)
{
    int i;

    if (!mt || !desc)
        return -EINVAL;

    memset(mt, 0, sizeof(*mt));
    mt->dev_addr = dev_addr;
    mt->present = 1;
    spinlock_init(&mt->lock);

    /* Initialise all field offsets to "not found" */
    mt->off_contact_count = -1;
    mt->off_contact_id    = -1;
    mt->off_x             = -1;
    mt->off_y             = -1;
    mt->off_width         = -1;
    mt->off_height        = -1;
    mt->off_pressure      = -1;
    mt->off_scan_time     = -1;
    mt->off_tip_switch    = -1;
    mt->off_confidence    = -1;

    /* Walk the parsed report descriptor items to detect
     * touchscreen/touchpad collections and extract capabilities. */
    for (i = 0; i < desc->num_items; i++) {
        const struct hid_report_item *ri = &desc->items[i];

        if (ri->tag == HID_ITEM_COLLECTION) {
            if (ri->collection_depth == 0 &&
                ri->global.usage_page == HID_PAGE_DIGITIZER) {
                if (ri->local.usage == HID_USAGE_TOUCH_SCREEN) {
                    mt->is_touchscreen = 1;
                    kprintf("[usb_hid_mt] Touch screen detected\n");
                } else if (ri->local.usage == HID_USAGE_TOUCH_PAD) {
                    mt->is_touchpad = 1;
                    kprintf("[usb_hid_mt] Touch pad detected\n");
                }
            }
        }

        /* Record capabilities from INPUT items */
        if (ri->tag == HID_ITEM_INPUT) {
            uint32_t up = ri->global.usage_page;
            uint32_t us = ri->local.usage;
            uint32_t umin = ri->local.usage_minimum;
            uint32_t umax = ri->local.usage_maximum;

            /* Check single usage */
            if (up == HID_PAGE_DIGITIZER) {
                if (us == HID_USAGE_CONTACT_ID)
                    mt->has_contact_id = 1;
                else if (us == HID_USAGE_TIP_SWITCH)
                    mt->has_tip_switch = 1;
                else if (us == HID_USAGE_CONFIDENCE)
                    mt->has_confidence = 1;
                else if (us == HID_USAGE_TIP_PRESSURE)
                    mt->has_pressure = 1;
                else if (us == HID_USAGE_WIDTH)
                    mt->has_width = 1;
                else if (us == HID_USAGE_HEIGHT)
                    mt->has_height = 1;
                else if (us == HID_USAGE_SCAN_TIME)
                    mt->has_scan_time = 1;
            }

            /* Check usage range */
            if (up == HID_PAGE_DIGITIZER && umin > 0 && umax > 0) {
                if (umin <= HID_USAGE_CONTACT_ID &&
                    umax >= HID_USAGE_CONTACT_ID)
                    mt->has_contact_id = 1;
                if (umin <= HID_USAGE_TIP_SWITCH &&
                    umax >= HID_USAGE_TIP_SWITCH)
                    mt->has_tip_switch = 1;
            }

            /* Record X/Y coordinate ranges */
            if (up == HID_PAGE_GENERIC_DESKTOP) {
                if (us == HID_USAGE_X) {
                    mt->x_min = ri->global.logical_minimum;
                    mt->x_max = ri->global.logical_maximum;
                }
                if (us == HID_USAGE_Y) {
                    mt->y_min = ri->global.logical_minimum;
                    mt->y_max = ri->global.logical_maximum;
                }
            }
        }
    }

    /* Determine if this is a multi-touch device */
    if (!mt->is_touchscreen && !mt->is_touchpad) {
        kprintf("[usb_hid_mt] Not a multi-touch device\n");
        return -ENODEV;
    }

    /* Compute precise field offsets for report processing */
    mt_compute_offsets(mt, desc);

    kprintf("[usb_hid_mt] Initialized: %s, %s, ranges (%d,%d)-(%d,%d)\n",
            mt->is_touchscreen ? "touchscreen" : "touchpad",
            mt->has_contact_id ? "multi-touch" : "single-touch",
            (int)mt->x_min, (int)mt->y_min,
            (int)mt->x_max, (int)mt->y_max);

    return 0;
}

int usb_hid_mt_init_raw(struct mt_device *mt, uint8_t dev_addr,
                          const uint8_t *rdesc, int rlen)
{
    struct hid_report_desc desc;
    int rc;

    if (!mt || !rdesc || rlen <= 0)
        return -EINVAL;

    memset(&desc, 0, sizeof(desc));
    rc = usb_hid_parse_report(NULL, rdesc, (size_t)rlen, &desc);
    if (rc < 0) {
        kprintf("[usb_hid_mt] Report descriptor parse failed (%d)\n", rc);
        return rc;
    }

    return usb_hid_mt_init(mt, dev_addr, &desc);
}

int usb_hid_mt_get_contact(struct mt_device *mt, int slot,
                            struct mt_contact *out)
{
    if (!mt || !out || slot < 0 || slot >= MT_MAX_CONTACTS)
        return -EINVAL;

    spinlock_acquire(&mt->lock);

    if (!mt->contacts[slot].active) {
        spinlock_release(&mt->lock);
        return -ENOENT;
    }

    memcpy(out, &mt->contacts[slot], sizeof(*out));
    spinlock_release(&mt->lock);
    return 0;
}

int usb_hid_mt_active_contacts(struct mt_device *mt)
{
    int n;
    if (!mt)
        return 0;
    spinlock_acquire(&mt->lock);
    n = mt->num_contacts;
    spinlock_release(&mt->lock);
    return n;
}

void usb_hid_mt_reset(struct mt_device *mt)
{
    if (!mt)
        return;

    spinlock_acquire(&mt->lock);
    memset(mt->contacts, 0, sizeof(mt->contacts));
    mt->num_contacts = 0;
    mt->contact_count = 0;
    spinlock_release(&mt->lock);
}

/* ── Global registration helpers ─────────────────────────────────────── */

static struct mt_device g_mt_devices[MT_MAX_DEVICES];
static int g_mt_count = 0;

int usb_hid_mt_register(uint8_t dev_addr, uint8_t input_ep,
                          const struct hid_report_desc *desc)
{
    if (g_mt_count >= MT_MAX_DEVICES)
        return -ENOSPC;

    struct mt_device *mt = &g_mt_devices[g_mt_count];
    int rc = usb_hid_mt_init(mt, dev_addr, desc);
    if (rc < 0)
        return rc;

    mt->input_ep = input_ep;
    mt->report_len = (desc->num_items > 0) ? 64 : 0;

    g_mt_count++;
    kprintf("[usb_hid_mt] Registered device %d at addr %d, ep 0x%x\n",
            g_mt_count - 1, dev_addr, input_ep);

    return g_mt_count - 1;
}

void usb_hid_mt_unregister(int mt_idx)
{
    if (mt_idx < 0 || mt_idx >= g_mt_count)
        return;

    struct mt_device *mt = &g_mt_devices[mt_idx];
    mt->present = 0;
    usb_hid_mt_reset(mt);
    kprintf("[usb_hid_mt] Device %d unregistered\n", mt_idx);
}

void usb_hid_mt_input(int mt_idx, const uint8_t *report, int len)
{
    if (mt_idx < 0 || mt_idx >= g_mt_count)
        return;

    struct mt_device *mt = &g_mt_devices[mt_idx];
    if (!mt->present)
        return;

    usb_hid_mt_process_report(mt, report, len);
}

int usb_hid_mt_get_count(void)
{
    return g_mt_count;
}

struct mt_device *usb_hid_mt_get_device(int idx)
{
    if (idx < 0 || idx >= g_mt_count)
        return NULL;
    return &g_mt_devices[idx];
}
