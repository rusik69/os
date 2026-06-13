#ifndef UHID_H
#define UHID_H

#include "types.h"

/* USB HID class protocol values */
#define USB_HID_PROTOCOL_BOOT  0
#define USB_HID_PROTOCOL_REPORT 1

/* HID descriptor types */
#define HID_DESC_HID           0x21
#define HID_DESC_REPORT        0x22

/* HID item tags (short items) */
#define HID_ITEM_INPUT         0x80
#define HID_ITEM_OUTPUT        0x90
#define HID_ITEM_FEATURE       0xB0
#define HID_ITEM_COLLECTION    0xA0
#define HID_ITEM_END_COLLECTION 0xC0
#define HID_ITEM_USAGE_PAGE    0x04
#define HID_ITEM_USAGE         0x08
#define HID_ITEM_LOGICAL_MIN   0x14
#define HID_ITEM_LOGICAL_MAX   0x24
#define HID_ITEM_REPORT_SIZE   0x74
#define HID_ITEM_REPORT_ID     0x84
#define HID_ITEM_REPORT_COUNT  0x94
#define HID_ITEM_PUSH          0xB4
#define HID_ITEM_POP           0xC4

/* Usage pages */
#define HID_PAGE_GENERIC_DESKTOP  0x01
#define HID_PAGE_KEYBOARD         0x07
#define HID_PAGE_BUTTONS          0x09

/* Generic Desktop usages */
#define HID_USAGE_KEYBOARD        0x06
#define HID_USAGE_MOUSE           0x02

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

/* USB HID driver API */
int usb_hid_init(void);
void usb_hid_poll(void);

/* Access HID keyboard state */
int  usb_hid_keyboard_present(void);
int  usb_hid_getchar(void);
int  usb_hid_has_input(void);

/* Access HID mouse state */
int  usb_hid_mouse_present(void);
void usb_hid_mouse_get(int *buttons, int *dx, int *dy);

#endif /* UHID_H */
