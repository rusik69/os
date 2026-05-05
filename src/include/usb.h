#ifndef USB_H
#define USB_H

#include "types.h"

/* USB device descriptor (simplified) */
struct usb_device {
    uint8_t  addr;
    uint8_t  speed;        /* 0=full, 1=low, 2=high */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
};

#define USB_CLASS_HID       0x03
#define USB_CLASS_MASS_STOR 0x08
#define USB_CLASS_HUB       0x09

#define USB_MAX_DEVICES 16

int  usb_init(void);              /* initialise all USB host controllers */
int  usb_is_present(void);
int  usb_get_device_count(void);
struct usb_device *usb_get_device(int idx);

/* EHCI internals exposed for the MSC transfer engine */
uint64_t ehci_get_op_base(void);
int      ehci_get_n_ports(void);

#endif
