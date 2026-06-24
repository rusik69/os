#ifndef USB_CORE_H
#define USB_CORE_H

#include "types.h"

/* ── USB device ID match flags ─────────────────────────────────── */

#define USB_DEVICE_ID_MATCH_VENDOR       (1U << 0)
#define USB_DEVICE_ID_MATCH_PRODUCT      (1U << 1)
#define USB_DEVICE_ID_MATCH_DEV_CLASS    (1U << 2)
#define USB_DEVICE_ID_MATCH_DEV_SUBCLASS (1U << 3)
#define USB_DEVICE_ID_MATCH_DEV_PROTOCOL (1U << 4)
#define USB_DEVICE_ID_MATCH_INT_CLASS    (1U << 5)

/* ── USB device ID structure ───────────────────────────────────── */

struct usb_device_id {
    uint16_t match_flags;
    uint16_t vendor;
    uint16_t product;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  protocol;
};

/* Convenience macro: match by VID:PID */
#define USB_DEVICE(vend, prod) \
    { .match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT, \
      .vendor = (vend), .product = (prod) }

/* Convenience macro: match by class/subclass/protocol */
#define USB_DEVICE_CLASS(cls, subcls, proto) \
    { .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS | \
                     USB_DEVICE_ID_MATCH_DEV_SUBCLASS | \
                     USB_DEVICE_ID_MATCH_DEV_PROTOCOL, \
      .class = (cls), .subclass = (subcls), .protocol = (proto) }

/* Terminator */
#define USB_DEVICE_TABLE_END \
    { .match_flags = 0, .vendor = 0 }

/* ── USB driver structure ──────────────────────────────────────── */

struct usb_driver {
    const char            *name;
    const struct usb_device_id *id_table;  /* NULL = match all */
    int (*probe)(const struct usb_device *dev);
    void (*disconnect)(const struct usb_device *dev);
    int refcount;  /* managed by core */
};

/* ── Core API ──────────────────────────────────────────────────── */

void usb_core_init(void);

int usb_register_driver(struct usb_driver *driver);
int usb_deregister_driver(struct usb_driver *driver);

int usb_core_add_device(const struct usb_device *desc);
int usb_core_remove_device(int device_index);
int usb_probe_device(int device_index);

int usb_core_get_device(int device_index, struct usb_device *out_desc);
struct usb_driver *usb_core_find_driver(const char *name);

#endif /* USB_CORE_H */
