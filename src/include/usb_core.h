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

/*
 * Host controller operations.
 * Each USB host controller (EHCI, xHCI, OHCI, UHCI) registers
 * an ops table with the core to provide transfer primitives.
 */
struct usb_hc_ops {
    /*
     * Submit a synchronous control transfer on endpoint 0.
     * Implements the full setup → data → status lifecycle.
     * @dev_addr:  USB device address
     * @setup:     8-byte setup packet (must remain valid until return)
     * @data:      data buffer (DMA-safe, may be NULL if wLength==0)
     * @len:       length of data buffer (= setup->wLength)
     * Returns 0 on success, negative errno on failure.
     */
    int (*control_transfer)(uint8_t dev_addr,
                            const struct usb_setup_packet *setup,
                            void *data, uint32_t len);
};

/* Register a host controller's ops table (called during HC init) */
int usb_register_hc_ops(const struct usb_hc_ops *ops);

/* Deregister host controller ops (called during HC shutdown) */
void usb_deregister_hc_ops(void);

/*
 * Submit a synchronous USB control transfer.
 * This is the primary API for USB class drivers to send control
 * requests to devices.  Handles the full setup packet lifecycle.
 *
 * @dev_addr:    USB device address
 * @bmReqType:   bmRequestType field (direction, type, recipient)
 * @bRequest:    bRequest field
 * @wValue:      wValue field
 * @wIndex:      wIndex field
 * @wLength:     wLength field (number of bytes in data stage)
 * @data:        data buffer (DMA-safe, may be NULL if wLength==0)
 *
 * Returns the number of bytes transferred on success,
 * or negative errno on failure.
 */
int usb_control_msg(uint8_t dev_addr, uint8_t bmReqType,
                    uint8_t bRequest, uint16_t wValue,
                    uint16_t wIndex, uint16_t wLength, void *data);

void usb_core_init(void);

int usb_register_driver(struct usb_driver *driver);
int usb_deregister_driver(struct usb_driver *driver);

int usb_core_add_device(const struct usb_device *desc);
int usb_core_remove_device(int device_index);
int usb_probe_device(int device_index);

int usb_core_get_device(int device_index, struct usb_device *out_desc);
struct usb_driver *usb_core_find_driver(const char *name);

#endif /* USB_CORE_H */
