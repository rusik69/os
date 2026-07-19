#ifndef UHID_H
#define UHID_H

#include "types.h"
#include "spinlock.h"
#include "ioctl.h"

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

/* ── Digitizer page usages (HID_PAGE_DIGITIZER = 0x0D) ────────────── */
#define HID_USAGE_TOUCH_SCREEN      0x04
#define HID_USAGE_TOUCH_PAD         0x05
#define HID_USAGE_TIP_SWITCH        0x42
#define HID_USAGE_TIP_PRESSURE      0x30
#define HID_USAGE_IN_RANGE          0x32
#define HID_USAGE_CONTACT_ID        0x51
#define HID_USAGE_CONTACT_COUNT     0x54
#define HID_USAGE_SCAN_TIME         0x56
#define HID_USAGE_CONFIDENCE        0x47
#define HID_USAGE_WIDTH             0x48
#define HID_USAGE_HEIGHT            0x49
#define HID_USAGE_AZIMUTH           0x57
#define HID_USAGE_ALTITUDE          0x58
#define HID_USAGE_TWIST             0x5B
#define HID_USAGE_DEVICE_INDEX      0x53
/* HID_USAGE_ACTUAL_COUNT shares value 0x54 with HID_USAGE_CONTACT_COUNT */

/* ── Multi-touch protocol constants ───────────────────────────────── */
#define MT_MAX_CONTACTS             10
#define MT_MAX_DEVICES              4

/*
 * Represents a single touch contact point.
 */
struct mt_contact {
    int      active;           /* 1 if this slot contains live data */
    uint8_t  contact_id;       /* HID Contact ID */
    int      tip;              /* Tip Switch (1=touching) */
    int      in_range;         /* In Range */
    int      confidence;       /* Confidence metric */
    int32_t  x;                /* X coordinate in device units */
    int32_t  y;                /* Y coordinate in device units */
    int32_t  pressure;         /* Tip pressure */
    int32_t  width;            /* Touch width */
    int32_t  height;           /* Touch height */
};

/*
 * Multi-touch device instance — tracks the state of one
 * touchscreen or touchpad.
 */
struct mt_device {
    int      present;
    spinlock_t lock;

    /* Contact tracking */
    struct mt_contact contacts[MT_MAX_CONTACTS];
    int      num_contacts;        /* number of active contacts */
    int      contact_count;       /* reported count from device */

    /* Device type */
    int      is_touchscreen;
    int      is_touchpad;

    /* Coordinate ranges (from Logical Min/Max of X/Y) */
    int32_t  x_min, x_max;
    int32_t  y_min, y_max;

    /* Report layout hints (set during descriptor parse) */
    int      has_contact_id;
    int      has_tip_switch;
    int      has_confidence;
    int      has_scan_time;
    int      has_pressure;
    int      has_width;
    int      has_height;

    /* USB addressing */
    uint8_t  dev_addr;
    uint8_t  input_ep;
    int      report_len;

    /* ── Field offsets (computed during descriptor parse) ──────────── */
    int      off_contact_count;   /* byte offset of Contact Count (or -1) */
    int      off_contact_id;      /* byte offset of Contact ID (or -1) */
    int      off_x;               /* byte offset of X (or -1) */
    int      off_y;               /* byte offset of Y (or -1) */
    int      off_width;           /* byte offset of Width (or -1) */
    int      off_height;          /* byte offset of Height (or -1) */
    int      off_pressure;        /* byte offset of Pressure (or -1) */
    int      off_scan_time;       /* byte offset of Scan Time (or -1) */
    int      size_x;              /* bit size of X field */
    int      size_y;              /* bit size of Y field */
    int      size_contact_id;     /* bit size of Contact ID */
    int      size_contact_count;  /* bit size of Contact Count */

    /* Tip Switch and Confidence are often 1-bit fields inside a byte.
     * Store both byte offset and bit mask. */
    int      off_tip_switch;      /* byte offset (or -1) */
    uint8_t  mask_tip_switch;     /* bit mask for Tip Switch */
    int      off_confidence;      /* byte offset (or -1) */
    uint8_t  mask_confidence;     /* bit mask for Confidence */

    /* Per-contact data size in bits (for parallel mode / grouping) */
    int      per_contact_bits;
};

/* ── Generic Desktop usages ──────────────────────────────────────── */
#define HID_USAGE_KEYBOARD          0x06
#define HID_USAGE_MOUSE             0x02
#define HID_USAGE_X                 0x30
#define HID_USAGE_Y                 0x31
#define HID_USAGE_JOYSTICK          0x04
#define HID_USAGE_GAMEPAD           0x05
#define HID_USAGE_MULTI_AXIS        0x08
#define HID_USAGE_TABLET_PC         0x0F
#define HID_USAGE_CONSUMER_CONTROL  0x01  /* from Consumer page */

/* ── Consumer Page usages (HID_PAGE_CONSUMER = 0x0C) ─────────────── */
/* Transport control */
#define HID_CONSUMER_PLAY            0xB0
#define HID_CONSUMER_PAUSE           0xB1
#define HID_CONSUMER_RECORD          0xB2
#define HID_CONSUMER_FAST_FORWARD    0xB3
#define HID_CONSUMER_REWIND          0xB4
#define HID_CONSUMER_SCAN_NEXT       0xB5
#define HID_CONSUMER_SCAN_PREVIOUS   0xB6
#define HID_CONSUMER_STOP            0xB7
#define HID_CONSUMER_EJECT           0xB8
#define HID_CONSUMER_PLAY_PAUSE      0xCD

/* Audio control */
#define HID_CONSUMER_VOLUME          0xE0
#define HID_CONSUMER_VOLUME_INCREMENT 0xE9
#define HID_CONSUMER_VOLUME_DECREMENT 0xEA
#define HID_CONSUMER_MUTE            0xE2
#define HID_CONSUMER_BASS            0xE3
#define HID_CONSUMER_TREBLE          0xE4
#define HID_CONSUMER_BASS_BOOST      0xE5
#define HID_CONSUMER_SURROUND_MODE   0xE6
#define HID_CONSUMER_LOUDNESS        0xE7
#define HID_CONSUMER_EQ              0xE8
#define HID_CONSUMER_BALANCE         0xE1  /* or Volume Balance */
#define HID_CONSUMER_FADE            0xEF  /* or Audio Fade */
#define HID_CONSUMER_MICROPHONE_MUTE 0xF8

/* Power control */
#define HID_CONSUMER_POWER           0x30
#define HID_CONSUMER_RESET           0x31
#define HID_CONSUMER_SLEEP           0x32

/* Menu/Navigation control */
#define HID_CONSUMER_MENU            0x40
#define HID_CONSUMER_MENU_PICK       0x41
#define HID_CONSUMER_MENU_UP         0x42
#define HID_CONSUMER_MENU_DOWN       0x43
#define HID_CONSUMER_MENU_LEFT       0x44
#define HID_CONSUMER_MENU_RIGHT      0x45
#define HID_CONSUMER_MENU_ESCAPE     0x46
#define HID_CONSUMER_MENU_VALUE_INC  0x47
#define HID_CONSUMER_MENU_VALUE_DEC  0x48

/* Display control */
#define HID_CONSUMER_DISPLAY_INFO    0x60
#define HID_CONSUMER_CLOSED_CAPTION  0x61
#define HID_CONSUMER_BRIGHTNESS_UP   0x6F
#define HID_CONSUMER_BRIGHTNESS_DOWN 0x70

/* ── System Control usages (Generic Desktop page 0x01) ──────────── */
#define HID_USAGE_SYSTEM_CONTROL       0x80
#define HID_USAGE_SYSTEM_POWER_DOWN    0x81
#define HID_USAGE_SYSTEM_SLEEP         0x82
#define HID_USAGE_SYSTEM_WAKE_UP       0x83
#define HID_USAGE_SYSTEM_COLD_RESTART  0x8E
#define HID_USAGE_SYSTEM_WARM_RESTART  0x8F

/* Application/Launch keys */
#define HID_CONSUMER_MEDIA_SELECT    0x183  /* AL Consumer Control Config */
#define HID_CONSUMER_EMAIL_READER    0x18A
#define HID_CONSUMER_CALCULATOR      0x192
#define HID_CONSUMER_MY_COMPUTER     0x194
#define HID_CONSUMER_WWW_HOME        0x223  /* AL Internet Browser */
#define HID_CONSUMER_WWW_SEARCH      0x221
#define HID_CONSUMER_WWW_FAVORITES   0x224
#define HID_CONSUMER_WWW_REFRESH     0x227
#define HID_CONSUMER_WWW_STOP        0x226
#define HID_CONSUMER_WWW_BACK        0x225
#define HID_CONSUMER_WWW_FORWARD     0x222
#define HID_CONSUMER_LAUNCH_DVD      0x228
#define HID_CONSUMER_LAUNCH_TV       0x229
#define HID_CONSUMER_LAUNCH_AUDIO    0x188
#define HID_CONSUMER_LAUNCH_VIDEO    0x189

/* Channel control */
#define HID_CONSUMER_CHANNEL_UP      0x9C
#define HID_CONSUMER_CHANNEL_DOWN    0x9D
#define HID_CONSUMER_AL_CHANNEL_INC  0x32B
#define HID_CONSUMER_AL_CHANNEL_DEC  0x32C

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

/* ── HID report descriptor limits ─────────────────────────────────── */
/* Maximum size of a HID report descriptor (65535 possible per USB HID spec,
 * but we impose a practical upper bound to prevent memory exhaustion and
 * parsing of malformed descriptors). */
#define HID_REPORT_DESC_MAX_SIZE     8192

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

/* ── Multi-touch API ──────────────────────────────────────────────── */

/*
 * Initialise multi-touch subsystem.
 * Scans the parsed report descriptor for touchscreen/touchpad collections
 * and configures the multi-touch device state accordingly.
 */
int usb_hid_mt_init(struct mt_device *mt, uint8_t dev_addr,
                     const struct hid_report_desc *desc);

/*
 * Convenience wrapper: parse a raw report descriptor and initialise
 * the multi-touch device from it.
 */
int usb_hid_mt_init_raw(struct mt_device *mt, uint8_t dev_addr,
                          const uint8_t *rdesc, int rlen);

/*
 * Register a multi-touch device with the global subsystem,
 * allocating a device index from the pool.
 * Returns the device index (>=0) or negative errno.
 */
int usb_hid_mt_register(uint8_t dev_addr, uint8_t input_ep,
                          const struct hid_report_desc *desc);

/*
 * Unregister a multi-touch device by index.
 */
void usb_hid_mt_unregister(int mt_idx);

/*
 * Feed a raw HID input report to a registered multi-touch device.
 */
void usb_hid_mt_input(int mt_idx, const uint8_t *report, int len);

/*
 * Process a raw HID input report from a multi-touch device.
 * Extracts per-contact data from the report and updates the
 * contact tracking state.
 */
void usb_hid_mt_process_report(struct mt_device *mt,
                                const uint8_t *report, int len);

/*
 * Read the current contact state for a given contact slot.
 * Returns 0 on success, -ENOENT if the slot has no active contact.
 * Internally synchronises via the device spinlock.
 */
int usb_hid_mt_get_contact(struct mt_device *mt, int slot,
                            struct mt_contact *out);

/*
 * Return the number of currently active contacts.
 */
int usb_hid_mt_active_contacts(struct mt_device *mt);

/*
 * Reset all contacts to inactive.
 */
void usb_hid_mt_reset(struct mt_device *mt);

/*
 * Get the number of registered multi-touch devices.
 */
int usb_hid_mt_get_count(void);

/*
 * Get a pointer to a registered multi-touch device by index.
 */
struct mt_device *usb_hid_mt_get_device(int idx);

/* ── Consumer Control (media keys) ──────────────────────────────────── */

#define MAX_CONSUMER_KEYS        64
#define CONSUMER_EVENT_QUEUE     32

/*
 * Consumer key event — reports press/release of a consumer-page key.
 * code is a HID_CONSUMER_* usage value from the Consumer page (0x0C).
 */
struct hid_consumer_event {
    uint16_t code;      /* HID_CONSUMER_* usage value */
    int      pressed;   /* 1 = pressed, 0 = released */
};

/*
 * Consumer device instance — tracks one consumer-control HID device.
 */
struct hid_consumer_dev {
    uint8_t  dev_addr;
    uint8_t  input_ep;
    uint8_t  intf_num;
    int      present;

    /* Report descriptor info */
    uint8_t  *report_desc;
    int       report_desc_len;
    int       report_len;        /* expected input report length */

    /* Event ring buffer */
    struct hid_consumer_event events[CONSUMER_EVENT_QUEUE];
    int      ev_head;
    int      ev_tail;

    spinlock_t lock;
};

/*
 * Register a consumer-control device.
 * Returns 0 on success, negative errno on failure.
 */
int usb_hid_consumer_register(uint8_t dev_addr, uint8_t intf_num,
                               uint8_t input_ep,
                               const uint8_t *report_desc, int desc_len);

/*
 * Unregister a consumer-control device.
 */
void usb_hid_consumer_unregister(void);

/*
 * Feed a raw HID input report to the consumer driver.
 * Parses consumer usages from the report and generates press/release events.
 */
void usb_hid_consumer_input(const uint8_t *report, int len);

/*
 * Check if the consumer driver is present and active.
 */
int usb_hid_consumer_present(void);

/*
 * Read the next pending consumer event (non-blocking).
 * Returns 1 if an event was read, 0 if queue empty.
 * The event is written to @out.
 */
int usb_hid_consumer_get_event(struct hid_consumer_event *out);

/*
 * Initialise the consumer subsystem (called once at boot).
 */
void usb_hid_consumer_init(void);

/*
 * Poll consumer device interrupt endpoint(s) and process reports.
 * Called from usb_hid_poll().
 */
void usb_hid_consumer_poll(void);

/*
 * Set a callback function that is called on each consumer key event.
 * Set to NULL to disable.  The callback is invoked with the event code
 * and pressed flag from interrupt context.
 */
void usb_hid_consumer_set_callback(void (*cb)(uint16_t code, int pressed));

/* ── System Control API ─────────────────────────────────────────────── */

#define SYSCTRL_EVENT_QUEUE     16

/*
 * System control event — reports press/release of a system control key.
 * code is a HID_USAGE_SYSTEM_* usage value from Generic Desktop page (0x01).
 */
struct hid_sysctrl_event {
    uint16_t code;      /* HID_USAGE_SYSTEM_* usage value */
    int      pressed;   /* 1 = pressed, 0 = released */
};

/*
 * System control device instance — tracks a system control HID device
 * that reports power, sleep, and wake key events.
 */
struct hid_sysctrl_dev {
    uint8_t  dev_addr;
    uint8_t  input_ep;
    uint8_t  intf_num;
    int      present;

    /* Report descriptor info */
    uint8_t  *report_desc;
    int       report_desc_len;
    int       report_len;        /* expected input report length */

    /* Event ring buffer */
    struct hid_sysctrl_event events[SYSCTRL_EVENT_QUEUE];
    int      ev_head;
    int      ev_tail;

    spinlock_t lock;
};

/*
 * Initialise the system control subsystem (called once at boot).
 */
void usb_hid_sysctrl_init(void);

/*
 * Register a system control device.
 * Returns 0 on success, negative errno on failure.
 */
int usb_hid_sysctrl_register(uint8_t dev_addr, uint8_t intf_num,
                              uint8_t input_ep,
                              const uint8_t *report_desc, int desc_len);

/*
 * Unregister a system control device.
 */
void usb_hid_sysctrl_unregister(void);

/*
 * Feed a raw HID input report to the system control driver.
 * Parses system control usages from the report and generates
 * press/release events.
 */
void usb_hid_sysctrl_input(const uint8_t *report, int len);

/*
 * Check if a system control device is present and active.
 */
int usb_hid_sysctrl_present(void);

/*
 * Read the next pending system control event (non-blocking).
 * Returns 1 if an event was read, 0 if queue empty.
 * The event is written to @out.
 */
int usb_hid_sysctrl_get_event(struct hid_sysctrl_event *out);

/*
 * Poll the system control device interrupt endpoint(s).
 * Called from usb_hid_poll().
 */
void usb_hid_sysctrl_poll(void);

/*
 * Set a callback function invoked on each system control key event.
 * Set to NULL to disable.  Called with the event code and pressed
 * flag from interrupt context.
 */
void usb_hid_sysctrl_set_callback(void (*cb)(uint16_t code, int pressed));

/* ── Gamepad / Joystick API ──────────────────────────────────────────── */

#define GAMEPAD_EVENT_QUEUE     32
#define GAMEPAD_MAX_AXES        64
#define GAMEPAD_MAX_BUTTONS     128
#define GAMEPAD_MAX_DEVICES     4

/*
 * Gamepad event types
 */
#define GAMEPAD_EV_KEY          1   /* Button press/release */
#define GAMEPAD_EV_ABS          2   /* Absolute axis change */
#define GAMEPAD_EV_HAT          3   /* Hat switch (POV) direction */

/*
 * Gamepad event — reports button presses, axis motion, and hat.
 */
struct hid_gamepad_event {
    int      type;       /* GAMEPAD_EV_KEY, _ABS, _HAT */
    int      code;       /* Key/axis/hat index */
    int32_t  value;      /* 1/0 for keys, raw for axes, direction for hat */
};

/*
 * Gamepad axis state with deadzone.
 */
struct hid_gamepad_axis {
    int32_t  min;
    int32_t  max;
    int32_t  value;
    int32_t  deadzone;
    int32_t  fuzz;
    int32_t  flat;
};

/*
 * Gamepad device instance — tracks one gamepad/joystick device.
 */
struct hid_gamepad_dev {
    uint8_t  dev_addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  input_ep;
    uint8_t  input_ep_interval;
    uint8_t  intf_num;
    int      present;
    int      is_gamepad;          /* 1 = Gamepad collection, 0 = Joystick */

    /* Report descriptor info */
    uint8_t  *report_desc;
    int       report_desc_len;
    int       report_len;

    /* Axis state */
    struct hid_gamepad_axis axes[GAMEPAD_MAX_AXES];
    int      n_axes;

    /* Button state */
    uint8_t  buttons[GAMEPAD_MAX_BUTTONS];
    int      n_buttons;

    /* Hat switch state (up to 4 hat switches) */
    int      hats[4];
    int      n_hats;

    /* Event ring buffer */
    struct hid_gamepad_event events[GAMEPAD_EVENT_QUEUE];
    int      ev_head;
    int      ev_tail;

    /* Rumble motor state */
    uint8_t  rumble_strong;
    uint8_t  rumble_weak;

    spinlock_t lock;
};

/*
 * Initialise the gamepad/joystick subsystem.
 * Called once at boot.
 */
int usb_hid_joy_init(void);

/*
 * Register a gamepad/joystick device.
 * Parses the HID report descriptor to detect axes, buttons, and hats.
 * @dev_addr:   USB device address
 * @vid:        USB vendor ID
 * @pid:        USB product ID
 * @intf_num:   interface number
 * @input_ep:   interrupt IN endpoint address
 * @interval:   endpoint polling interval (in frames)
 * @report_desc: raw HID report descriptor
 * @desc_len:   length of report descriptor
 * Returns device index (>=0) on success, negative errno on failure.
 */
int usb_hid_joy_register(uint8_t dev_addr, uint16_t vid, uint16_t pid,
                          uint8_t intf_num, uint8_t input_ep,
                          uint8_t interval,
                          const uint8_t *report_desc, int desc_len);

/*
 * Unregister a gamepad/joystick device.
 */
void usb_hid_joy_unregister(int joy_idx);

/*
 * Feed a raw HID input report to a registered gamepad device.
 * Parses axes, buttons, and hats from the report and generates events.
 */
void usb_hid_joy_input(int joy_idx, const uint8_t *report, int len);

/*
 * Read the current axis value (raw, no deadzone applied).
 */
int usb_hid_joy_get_axis(int joy_idx, int axis);

/*
 * Read the current button state (1 = pressed, 0 = released).
 */
int usb_hid_joy_get_button(int joy_idx, int btn);

/*
 * Return the number of axes for a given device.
 */
int usb_hid_joy_get_axis_count(int joy_idx);

/*
 * Return the number of buttons for a given device.
 */
int usb_hid_joy_get_button_count(int joy_idx);

/*
 * Return the number of registered gamepad devices.
 */
int usb_hid_joy_get_count(void);

/*
 * Return the number of hat switches for a given device.
 */
int usb_hid_joy_get_hat_count(int joy_idx);

/*
 * Read the current hat switch direction.
 * Returns 0=centered, 1=up, 2=right, 3=down, 4=left, or
 * combined diagonals: 5=up-right, 6=down-right, 7=down-left, 8=up-left.
 */
int usb_hid_joy_get_hat(int joy_idx, int hat_idx);

/*
 * Read the next pending gamepad event (non-blocking).
 * Returns 1 if an event was read, 0 if the queue is empty.
 * The event is written to @out.
 */
int usb_hid_joy_get_event(int joy_idx, struct hid_gamepad_event *out);

/*
 * Poll a gamepad device's interrupt endpoint.
 * Called from the USB HID poll loop.
 */
void usb_hid_joy_poll(int joy_idx);

/*
 * Poll all registered gamepad devices.
 */
void usb_hid_joy_poll_all(void);

/*
 * Set a callback function invoked on each gamepad event.
 * Set to NULL to disable.  Called from interrupt context with
 * a pointer to the event structure (valid only during callback).
 */
void usb_hid_joy_set_callback(int joy_idx,
                               void (*cb)(struct hid_gamepad_event *ev));

/*
 * Set rumble motor intensities (force feedback).
 * @strong: low-frequency motor (0–255)
 * @weak:   high-frequency motor (0–255)
 * Returns 0 on success, negative errno on failure or if unsupported.
 */
int usb_hid_joy_set_rumble(int joy_idx, uint8_t strong, uint8_t weak);

/*
 * Set the deadzone for a specific axis (in raw units).
 * Axis values within deadzone of centre are snapped to centre.
 * Pass axis = -1 to set deadzone for all axes.
 */
int usb_hid_joy_set_deadzone(int joy_idx, int axis, int32_t deadzone);

/*
 * Read raw input report data (for /dev/input/js* style access).
 * Copies the current full gamepad state into @buf (struct layout).
 * Returns number of bytes written, or negative errno.
 */
int usb_hid_joy_read(int joy_idx, void *buf, size_t count);

/*
 * Gamepad-specific ioctl commands.
 * Returns negative errno on failure.
 */
int usb_hid_joy_ioctl(int joy_idx, int cmd, void *arg);

/* Gamepad ioctl command codes */
#define JOYIOC_GAXES        _IOR('J', 0, int)    /* get number of axes */
#define JOYIOC_GBUTTONS     _IOR('J', 1, int)    /* get number of buttons */
#define JOYIOC_GHATS        _IOR('J', 2, int)    /* get number of hats */
#define JOYIOC_GDEADZONE    _IOR('J', 3, int)    /* get deadzone */
#define JOYIOC_SDEADZONE    _IOW('J', 3, int)    /* set deadzone */
#define JOYIOC_GRUMBLE      _IOR('J', 4, int)    /* get rumble capability */
#define JOYIOC_SRUMBLE      _IOW('J', 5, struct joy_rumble) /* set rumble */
#define JOYIOC_GAME_PAD     _IOR('J', 6, int)    /* get 1 if gamepad, 0 if joystick */
#define JOYIOC_CALIBRATE    _IOW('J', 7, int)    /* trigger auto-calibration */

/* Rumble parameters for JOYIOC_SRUMBLE */
struct joy_rumble {
    uint8_t strong;
    uint8_t weak;
};

/* ── HID Gamepad-specific usage page values (from USB HID Usage Tables) ─ */
/* Simulation Page (0x02) — additional values not yet defined */
#ifndef HID_PAGE_SIMULATION
#define HID_PAGE_SIMULATION         0x02
#endif
#define HID_USAGE_FLIGHT_SIM        0x01
#define HID_USAGE_AUTOMOBILE_SIM    0x02
#define HID_USAGE_TANK_SIM          0x03
#define HID_USAGE_SPACESHIP_SIM     0x04
#define HID_USAGE_SUBMARINE_SIM     0x05
#define HID_USAGE_RUDDER            0xBA
#define HID_USAGE_THROTTLE          0xBB

/* Generic Desktop Hat Switch */
#define HID_USAGE_HAT_SWITCH        0x39

/* Generic Desktop additional usages not yet defined */
#define HID_USAGE_KEYPAD            0x07

/* Gamepad specific button codes (Linux evdev style for user mapping) */
#define GAMEPAD_BTN_A               0
#define GAMEPAD_BTN_B               1
#define GAMEPAD_BTN_X               2
#define GAMEPAD_BTN_Y               3
#define GAMEPAD_BTN_LB              4
#define GAMEPAD_BTN_RB              5
#define GAMEPAD_BTN_LT              6
#define GAMEPAD_BTN_RT              7
#define GAMEPAD_BTN_SELECT          8
#define GAMEPAD_BTN_START           9
#define GAMEPAD_BTN_L3              10
#define GAMEPAD_BTN_R3              11
#define GAMEPAD_BTN_HOME            12
#define GAMEPAD_BTN_GUIDE           12      /* alias */

#endif /* UHID_H */
