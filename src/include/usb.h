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

#define USB_CLASS_HID        0x03
#define USB_CLASS_MASS_STOR  0x08
#define USB_CLASS_HUB        0x09
#define USB_CLASS_AUDIO      0x01   /* USB Audio class */
#define USB_CLASS_AUDIO_CTRL 0x01   /* Audio control interface */
#define USB_CLASS_AUDIO_STREAM 0x02 /* Audio streaming interface (actually subclass) */

#define USB_MAX_DEVICES 16

int  usb_init(void);              /* initialise all USB host controllers */
int  usb_is_present(void);
int  usb_get_device_count(void);
struct usb_device *usb_get_device(int idx);

/* EHCI internals exposed for the MSC transfer engine */
uint64_t ehci_get_op_base(void);
int      ehci_get_n_ports(void);

/*
 * Isochronous transfer support (USB Audio class & similar).
 *
 * EHCI uses the periodic schedule with iTD (isochronous Transfer Descriptor)
 * entries.  The frame list is a 1024-entry × 4-byte array at a page-aligned
 * physical address programmed into the PERIODICBASE register.
 */

/* Max bytes per isochronous packet (one micro-frame) */
#define USB_ISO_MAX_PACKET  1024

/* Max micro-frames per iTD (EHCI: 8 µframes per 1ms frame) */
#define USB_ISO_MAX_UCT    8

/**
 * ehci_setup_periodic() — Allocate periodic frame list and enable periodic
 * schedule on the first EHCI controller.
 * Returns 0 on success, negative on error.
 */
int ehci_setup_periodic(void);

/**
 * ehci_teardown_periodic() — Disable periodic schedule and free frame list.
 */
void ehci_teardown_periodic(void);

/**
 * ehci_submit_isochronous() — Synchronous isochronous transfer on the
 * periodic schedule.
 * @dev_addr:  USB device address
 * @ep:        Endpoint number (0x80|n for IN, n for OUT)
 * @buf:       Data buffer (must be permanently mapped, not stack)
 * @len:       Transfer length in bytes (≤ USB_ISO_MAX_PACKET)
 * @sched_frame: Frame index modulo 1024 for bus-time alignment
 * Returns 0 on success, negative on error.
 */
int ehci_submit_isochronous(uint8_t dev_addr, uint8_t ep,
                            void *buf, uint32_t len,
                            uint32_t sched_frame);

/* xHCI isochronous stub (returns -ENOSYS for now) */
int xhci_submit_isochronous(uint8_t dev_addr, uint8_t ep,
                             void *buf, uint32_t len);

#endif
