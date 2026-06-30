#ifndef UHID_H
#define UHID_H

#include "types.h"

/* USB HID class protocol values */
#define USB_HID_PROTOCOL_BOOT   0
#define USB_HID_PROTOCOL_REPORT 1

/* HID descriptor types */
#define HID_DESC_HID           0x21
#define HID_DESC_REPORT        0x22

/* ── HID class-specific requests (USB HID Spec §7.2) ──────────────── */
#define HID_REQ_GET_REPORT      0x01
#define HID_REQ_GET_IDLE        0x02
#define HID_REQ_GET_PROTOCOL    0x03
#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

/* ── HID report types (for GET/SET_REPORT bRequest type) ─────────── */
#define HID_REPORT_INPUT        1
#define HID_REPORT_OUTPUT       2
#define HID_REPORT_FEATURE      3

/* ── HID request type byte builder ────────────────────────────────── */
/* Host-to-device, Class, Interface */
#define HID_REQTYPE_SET  ((USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE))
/* Device-to-host, Class, Interface */
#define HID_REQTYPE_GET  ((USB_DIR_IN  | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE))

/* ── Boot protocol output report (keyboard LEDs) ──────────────────── */
/* SET_REPORT output report bit definitions (1-byte report) */
#define HID_BOOT_LED_NUM_LOCK     (1U << 0)
#define HID_BOOT_LED_CAPS_LOCK    (1U << 1)
#define HID_BOOT_LED_SCROLL_LOCK  (1U << 2)
#define HID_BOOT_LED_COMPOSE      (1U << 3)
#define HID_BOOT_LED_KANA         (1U << 4)

/* ── HID item type (bType) ───────────────────────────────────────── */
#define HID_TYPE_MAIN          0
#define HID_TYPE_GLOBAL        1
#define HID_TYPE_LOCAL         2
#define HID_TYPE_RESERVED      3

/* ── Short item bSize encoding ───────────────────────────────────── */
/* bSize: 0 = 0 bytes, 1 = 1 byte, 2 = 2 bytes, 3 = 4 bytes */

/* ── Main item tags (bTag in Main type) ──────────────────────────── */
#define HID_ITEM_INPUT               0x80  /* bTag=8,  type=0 */
#define HID_ITEM_OUTPUT              0x90  /* bTag=9,  type=0 */
#define HID_ITEM_COLLECTION          0xA0  /* bTag=10, type=0 */
#define HID_ITEM_FEATURE             0xB0  /* bTag=11, type=0 */
#define HID_ITEM_END_COLLECTION     0xC0  /* bTag=12, type=0 */

/* ── Input/Output/Feature item flags ─────────────────────────────── */
#define HID_IOF_DATA                (0u << 0)  /* Data field */
#define HID_IOF_CONST               (1u << 0)  /* Constant field */
#define HID_IOF_ARRAY               (0u << 1)  /* Array (variable count) */
#define HID_IOF_VARIABLE            (1u << 1)  /* Variable (one per field) */
#define HID_IOF_ABSOLUTE            (0u << 2)  /* Absolute coordinate */
#define HID_IOF_RELATIVE            (1u << 2)  /* Relative coordinate */
#define HID_IOF_NO_WRAP             (0u << 3)  /* No wrap */
#define HID_IOF_WRAP                (1u << 3)  /* Wrap */
#define HID_IOF_LINEAR              (0u << 4)  /* Linear */
#define HID_IOF_NON_LINEAR          (1u << 4)  /* Non-linear */
#define HID_IOF_PREFERRED           (0u << 5)  /* Preferred state */
#define HID_IOF_NO_PREFERRED        (1u << 5)  /* No preferred */
#define HID_IOF_NO_NULL             (0u << 6)  /* No null position */
#define HID_IOF_NULL_STATE          (1u << 6)  /* Null state */
#define HID_IOF_NON_VOLATILE        (0u << 7)  /* Non-volatile */
#define HID_IOF_VOLATILE            (1u << 7)  /* Volatile */
#define HID_IOF_BIT_FIELD           (0u << 8)  /* Bit field */
#define HID_IOF_BUFFERED_BYTES      (1u << 8)  /* Buffered bytes */

/* ── Collection types ────────────────────────────────────────────── */
#define HID_COLLECTION_PHYSICAL     0x00
#define HID_COLLECTION_APPLICATION  0x01
#define HID_COLLECTION_LOGICAL      0x02
#define HID_COLLECTION_REPORT       0x03
#define HID_COLLECTION_NAMED_ARRAY  0x04
#define HID_COLLECTION_USAGE_SWITCH 0x05
#define HID_COLLECTION_USAGE_MODIFIER 0x06

/* ── Global item tags (bTag in Global type) ──────────────────────── */
#define HID_ITEM_USAGE_PAGE         0x04  /* bTag=0,  type=1 */
#define HID_ITEM_LOGICAL_MIN        0x14  /* bTag=1,  type=1 */
#define HID_ITEM_LOGICAL_MAX        0x24  /* bTag=2,  type=1 */
#define HID_ITEM_PHYSICAL_MIN       0x34  /* bTag=3,  type=1 */
#define HID_ITEM_PHYSICAL_MAX       0x44  /* bTag=4,  type=1 */
#define HID_ITEM_UNIT_EXPONENT      0x54  /* bTag=5,  type=1 */
#define HID_ITEM_UNIT               0x64  /* bTag=6,  type=1 */
#define HID_ITEM_REPORT_SIZE        0x74  /* bTag=7,  type=1 */
#define HID_ITEM_REPORT_ID          0x84  /* bTag=8,  type=1 */
#define HID_ITEM_REPORT_COUNT       0x94  /* bTag=9,  type=1 */
#define HID_ITEM_PUSH               0xA4  /* bTag=10, type=1 */
#define HID_ITEM_POP                0xB4  /* bTag=11, type=1 */

/* ── Local item tags (bTag in Local type) ────────────────────────── */
#define HID_ITEM_USAGE              0x08  /* bTag=0, type=2 */
#define HID_ITEM_USAGE_MINIMUM      0x18  /* bTag=1, type=2 */
#define HID_ITEM_USAGE_MAXIMUM      0x28  /* bTag=2, type=2 */
#define HID_ITEM_DESIGNATOR_INDEX   0x38  /* bTag=3, type=2 */
#define HID_ITEM_DESIGNATOR_MINIMUM 0x48  /* bTag=4, type=2 */
#define HID_ITEM_DESIGNATOR_MAXIMUM 0x58  /* bTag=5, type=2 */
#define HID_ITEM_STRING_INDEX       0x68  /* bTag=6, type=2 */
#define HID_ITEM_STRING_MINIMUM     0x78  /* bTag=7, type=2 */
#define HID_ITEM_STRING_MAXIMUM     0x88  /* bTag=8, type=2 */
#define HID_ITEM_DELIMITER          0x98  /* bTag=9, type=2 */

/* ── Long item prefix ────────────────────────────────────────────── */
#define HID_LONG_ITEM_PREFIX        0xFE

/* ── Usage pages ─────────────────────────────────────────────────── */
#define HID_PAGE_GENERIC_DESKTOP    0x01
#define HID_PAGE_SIMULATION         0x02
#define HID_PAGE_VR_CONTROLS        0x03
#define HID_PAGE_SPORT              0x04
#define HID_PAGE_GAME               0x05
#define HID_PAGE_KEYBOARD           0x07
#define HID_PAGE_LEDS               0x08
#define HID_PAGE_BUTTONS            0x09
#define HID_PAGE_ORDINAL            0x0A
#define HID_PAGE_TELEPHONY          0x0B
#define HID_PAGE_CONSUMER           0x0C
#define HID_PAGE_DIGITIZER          0x0D
#define HID_PAGE_PID                0x0F
#define HID_PAGE_UNICODE            0x10
#define HID_PAGE_ALPHANUMERIC       0x14
#define HID_PAGE_MEDICAL            0x40
#define HID_PAGE_MONITOR            0x80
#define HID_PAGE_POWER              0x84
#define HID_PAGE_BARCODE            0x8C
#define HID_PAGE_SCALE              0x8D
#define HID_PAGE_MSR                0x8E
#define HID_PAGE_CAMERA             0x90
#define HID_PAGE_ARCADE             0x91
#define HID_PAGE_VENDOR_MS_BEGIN    0xFF00
#define HID_PAGE_VENDOR_MS_END      0xFFFF

/* ── Generic Desktop usages ──────────────────────────────────────── */
#define HID_USAGE_KEYBOARD          0x06
#define HID_USAGE_MOUSE             0x02
#define HID_USAGE_JOYSTICK          0x04
#define HID_USAGE_GAMEPAD           0x05
#define HID_USAGE_MULTI_AXIS        0x08
#define HID_USAGE_TABLET_PC         0x0F
#define HID_USAGE_CONSUMER_CONTROL  0x01  /* from Consumer page */

/* Boot protocol keyboard report (8 bytes) */
struct hid_keyboard_report {
    uint8_t modifiers;  /* bitmask: LCtrl=1, LShift=2, LAlt=4, LGui=8, RCtrl=16, RShift=32, RAlt=64, RGui=128 */
    uint8_t reserved;
    uint8_t keys[6];    /* keycodes of currently pressed keys (max 6) */
} __attribute__((packed));

/* Boot protocol mouse report (4 bytes) */
struct hid_mouse_report {
    uint8_t buttons;    /* bitmask: left=1, right=2, middle=4 */
    int8_t  x_delta;    /* X movement (signed) */
    int8_t  y_delta;    /* Y movement (signed) */
    int8_t  wheel;      /* wheel movement (signed) */
} __attribute__((packed));

/* HID usage to PS/2 scancode mapping (boot protocol subset) */
#define HID_KEYCODE_NONE      0x00
#define HID_KEYCODE_ERROR     0x01
#define HID_KEYCODE_A         0x04
#define HID_KEYCODE_B         0x05
#define HID_KEYCODE_C         0x06
#define HID_KEYCODE_D         0x07
#define HID_KEYCODE_E         0x08
#define HID_KEYCODE_F         0x09
#define HID_KEYCODE_G         0x0A
#define HID_KEYCODE_H         0x0B
#define HID_KEYCODE_I         0x0C
#define HID_KEYCODE_J         0x0D
#define HID_KEYCODE_K         0x0E
#define HID_KEYCODE_L         0x0F
#define HID_KEYCODE_M         0x10
#define HID_KEYCODE_N         0x11
#define HID_KEYCODE_O         0x12
#define HID_KEYCODE_P         0x13
#define HID_KEYCODE_Q         0x14
#define HID_KEYCODE_R         0x15
#define HID_KEYCODE_S         0x16
#define HID_KEYCODE_T         0x17
#define HID_KEYCODE_U         0x18
#define HID_KEYCODE_V         0x19
#define HID_KEYCODE_W         0x1A
#define HID_KEYCODE_X         0x1B
#define HID_KEYCODE_Y         0x1C
#define HID_KEYCODE_Z         0x1D
#define HID_KEYCODE_1         0x1E
#define HID_KEYCODE_2         0x1F
#define HID_KEYCODE_3         0x20
#define HID_KEYCODE_4         0x21
#define HID_KEYCODE_5         0x22
#define HID_KEYCODE_6         0x23
#define HID_KEYCODE_7         0x24
#define HID_KEYCODE_8         0x25
#define HID_KEYCODE_9         0x26
#define HID_KEYCODE_0         0x27
#define HID_KEYCODE_ENTER     0x28
#define HID_KEYCODE_ESC       0x29
#define HID_KEYCODE_BACKSPACE 0x2A
#define HID_KEYCODE_TAB       0x2B
#define HID_KEYCODE_SPACE     0x2C
#define HID_KEYCODE_MINUS     0x2D
#define HID_KEYCODE_EQUAL     0x2E
#define HID_KEYCODE_LBRACKET  0x2F
#define HID_KEYCODE_RBRACKET  0x30
#define HID_KEYCODE_BSLASH    0x31
#define HID_KEYCODE_SEMICOLON 0x33
#define HID_KEYCODE_QUOTE     0x34
#define HID_KEYCODE_GRAVE     0x35
#define HID_KEYCODE_COMMA     0x36
#define HID_KEYCODE_DOT       0x37
#define HID_KEYCODE_SLASH     0x38
#define HID_KEYCODE_CAPSLOCK  0x39
#define HID_KEYCODE_F1        0x3A
#define HID_KEYCODE_F2        0x3B
#define HID_KEYCODE_F3        0x3C
#define HID_KEYCODE_F4        0x3D
#define HID_KEYCODE_F5        0x3E
#define HID_KEYCODE_F6        0x3F
#define HID_KEYCODE_F7        0x40
#define HID_KEYCODE_F8        0x41
#define HID_KEYCODE_F9        0x42
#define HID_KEYCODE_F10       0x43
#define HID_KEYCODE_F11       0x44
#define HID_KEYCODE_F12       0x45
#define HID_KEYCODE_UP        0x52
#define HID_KEYCODE_DOWN      0x51
#define HID_KEYCODE_LEFT      0x50
#define HID_KEYCODE_RIGHT     0x4F

/* ── HID report descriptor parser structures ──────────────────────── */

#define HID_REPORT_MAX_ITEMS         64
#define HID_REPORT_MAX_COLLECTIONS   16
#define HID_GLOBAL_STACK_DEPTH       4

/*
 * HID global item state — tracks the current global items that apply
 * to subsequent Main items in the report descriptor.
 */
struct hid_global_state {
    uint32_t usage_page;
    int32_t  logical_minimum;
    int32_t  logical_maximum;
    int32_t  physical_minimum;
    int32_t  physical_maximum;
    uint32_t unit;
    uint32_t unit_exponent;
    uint32_t report_size;
    uint32_t report_id;
    uint32_t report_count;
};

/*
 * HID local item state — tracks the current local items that apply
 * to the immediately following Main item.  Resets to defaults
 * after each Main item.
 */
struct hid_local_state {
    uint32_t usage;
    uint32_t usage_minimum;
    uint32_t usage_maximum;
    uint32_t designator_index;
    uint32_t designator_minimum;
    uint32_t designator_maximum;
    uint32_t string_index;
    uint32_t string_minimum;
    uint32_t string_maximum;
    uint32_t delimiter;
};

/*
 * HID collection — tracks a collection start in the descriptor.
 * Collections form a tree structure via nesting in the descriptor stream.
 */
struct hid_collection {
    uint8_t  type;          /* HID_COLLECTION_* */
    uint32_t usage_page;    /* usage page at time of collection start */
    uint32_t usage;         /* usage at time of collection start */
};

/*
 * HID parsed report item — represents one Main item (Input/Output/
 * Feature/Collection) with its associated global and local state.
 */
struct hid_report_item {
    uint8_t  tag;                    /* HID_ITEM_INPUT / OUTPUT / FEATURE / COLLECTION */
    uint32_t flags;                  /* for Input/Output/Feature: IOF_* flags */
    uint32_t data;                   /* for Collection: the collection type value */
    struct hid_global_state global;  /* global state snapshot at this item */
    struct hid_local_state  local;   /* local state at this item */
    int      collection_depth;       /* collection nesting depth at this item */
};

/*
 * HID parsed report descriptor — the fully parsed representation
 * of a HID Report Descriptor.
 */
struct hid_report_desc {
    struct hid_report_item items[HID_REPORT_MAX_ITEMS];
    int num_items;

    struct hid_collection collections[HID_REPORT_MAX_COLLECTIONS];
    int num_collections;

    /* collection stack for tracking nesting during parsing */
    int collection_stack[HID_REPORT_MAX_COLLECTIONS];
    int collection_stack_depth;

    struct hid_global_state global_stack[HID_GLOBAL_STACK_DEPTH];
    int global_stack_depth;
};

/* ── HID descriptor structure (USB HID Spec §6.2.1, 9 bytes) ──────── */
struct hid_descriptor {
    uint8_t  bLength;            /* 9 bytes */
    uint8_t  bDescriptorType;    /* HID_DESC_HID (0x21) */
    uint16_t bcdHID;             /* HID specification release (BCD) */
    uint8_t  bCountryCode;       /* country code of the localized hardware */
    uint8_t  bNumDescriptors;    /* number of subordinate report/other descriptors */
    uint8_t  bDescriptorType0;   /* type of first subordinate descriptor */
    uint16_t wDescriptorLength0; /* total length of the first subordinate descriptor */
} __attribute__((packed));

/* ── Parser API ──────────────────────────────────────────────────── */

/*
 * Parse a raw HID Report Descriptor into a structured representation.
 *
 * @dev:      USB device pointer (may be NULL)
 * @report:   pointer to the raw report descriptor data
 * @len:      length of the report descriptor in bytes
 * @out:      [out] parsed report descriptor structure
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_hid_parse_report(void *dev, const void *report, size_t len,
                         struct hid_report_desc *out);

/* USB HID driver API */
int usb_hid_init(void);
void usb_hid_poll(void);

/* Access HID keyboard state */
int  usb_hid_keyboard_present(void);
int  usb_hid_getchar(void);
int  usb_hid_has_input(void);

/* Set keyboard LEDs (bitmask of HID_BOOT_LED_*) via SET_REPORT */
int usb_hid_set_leds(uint8_t leds);

/* Access HID mouse state */
int  usb_hid_mouse_present(void);
void usb_hid_mouse_get(int *buttons, int *dx, int *dy);

/* Get mouse wheel delta (accumulated, resets on read) */
int  usb_hid_mouse_wheel_get(void);

/* ── Boot protocol control transfers ─────────────────────────────── */
int usb_hid_get_report(uint8_t report_type, uint8_t report_id,
                       void *buf, size_t len);
int usb_hid_set_report(uint8_t report_type, uint8_t report_id,
                       const void *buf, size_t len);

/* Fetch the HID descriptor from the device */
int usb_hid_get_hid_descriptor(struct hid_descriptor *desc);

/* Fetch the HID report descriptor and parse it */
int usb_hid_get_and_parse_report_descriptor(void);

#endif /* UHID_H */
