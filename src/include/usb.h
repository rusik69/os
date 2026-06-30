#ifndef USB_H
#define USB_H

#include "types.h"

/* ── USB descriptor type constants (USB 2.0 spec §9.6) ──────────────── */
#define USB_DT_DEVICE             1
#define USB_DT_CONFIG             2
#define USB_DT_STRING             3
#define USB_DT_INTERFACE          4
#define USB_DT_ENDPOINT           5
#define USB_DT_DEVICE_QUALIFIER   6
#define USB_DT_OTHER_SPEED_CONFIG 7
#define USB_DT_INTERFACE_POWER    8
#define USB_DT_OTG                9
#define USB_DT_DEBUG             10
#define USB_DT_INTERFACE_ASSOC   11
#define USB_DT_BOS               15
#define USB_DT_DEVICE_CAPABILITY 16
#define USB_DT_HID               33
#define USB_DT_REPORT            34
#define USB_DT_PHYSICAL          35
#define USB_DT_HUB               41
#define USB_DT_SS_ENDPOINT_COMP  48

/* ── Standard request type bits (bmRequestType) ──────────────────────── */
#define USB_REQ_TYPE_STANDARD   (0x00 << 5)
#define USB_REQ_TYPE_CLASS      (0x01 << 5)
#define USB_REQ_TYPE_VENDOR     (0x02 << 5)
#define USB_REQ_TYPE_MASK       (0x60)

#define USB_REQ_RECIP_DEVICE    0x00
#define USB_REQ_RECIP_INTERFACE 0x01
#define USB_REQ_RECIP_ENDPOINT  0x02
#define USB_REQ_RECIP_OTHER     0x03
#define USB_REQ_RECIP_MASK      0x1F

#define USB_DIR_OUT             0x00
#define USB_DIR_IN              0x80

/* ── Standard USB requests (bRequest) ────────────────────────────────── */
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_REQ_SYNCH_FRAME         0x0C

/* ── USB device descriptor (USB 2.0 spec §9.6.1, 18 bytes) ──────────── */
struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* ── USB configuration descriptor (USB 2.0 spec §9.6.3, 9 bytes) ─────── */
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* ── USB interface descriptor (USB 2.0 spec §9.6.5, 9 bytes) ─────────── */
struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* ── USB endpoint descriptor (USB 2.0 spec §9.6.6, 7 bytes) ──────────── */
struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* ── USB string descriptor (variable length) ─────────────────────────── */
struct usb_string_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wData[];
} __attribute__((packed));

/* ── Endpoint address/attribute decode constants ─────────────────────── */
#define USB_ENDPOINT_DIR_MASK      0x80
#define USB_ENDPOINT_DIR_IN        0x80
#define USB_ENDPOINT_DIR_OUT       0x00
#define USB_ENDPOINT_NUMBER_MASK   0x0F
#define USB_ENDPOINT_XFERTYPE_MASK 0x03
#define USB_ENDPOINT_XFER_CONTROL  0x00
#define USB_ENDPOINT_XFER_ISOCH    0x01
#define USB_ENDPOINT_XFER_BULK     0x02
#define USB_ENDPOINT_XFER_INT      0x03

/* ── Standard feature selectors ──────────────────────────────────────── */
#define USB_FEATURE_ENDPOINT_HALT         0
#define USB_FEATURE_DEVICE_REMOTE_WAKEUP  1
#define USB_FEATURE_TEST_MODE             2

/* ── USB device flags ────────────────────────────────────────────────── */
#define USB_DEV_FLAG_HAS_DESC      (1U << 0)   /* device descriptor parsed */
#define USB_DEV_FLAG_HAS_CONFIG    (1U << 1)   /* config descriptor parsed */
#define USB_DEV_FLAG_HAS_STRINGS   (1U << 2)   /* string descriptors read */

/* ── Expanded USB device structure ────────────────────────────────────── */
struct usb_device {
    uint8_t  addr;
    uint8_t  speed;        /* 0=full, 1=low, 2=high */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  _rsvd;        /* reserved for alignment */
    struct usb_device_descriptor dev_desc;   /* full parsed descriptor */
    uint32_t flags;
};

#define USB_CLASS_HID        0x03
#define USB_CLASS_MASS_STOR  0x08
#define USB_CLASS_HUB        0x09
#define USB_CLASS_AUDIO      0x01   /* USB Audio class */
#define USB_CLASS_AUDIO_CTRL 0x01   /* Audio control interface */
#define USB_CLASS_AUDIO_STREAM 0x02 /* Audio streaming interface (actually subclass) */

#define USB_MAX_DEVICES 16

int  usb_init(void);              /* initialise all USB host controllers */
void usb_exit(void);              /* shutdown all USB host controllers */
int  usb_is_present(void);
int  usb_get_device_count(void);
struct usb_device *usb_get_device(int idx);

/* ── Descriptor parsing API ──────────────────────────────────────────── */
int usb_parse_device_descriptor(const uint8_t *raw,
                                struct usb_device_descriptor *desc);
int usb_parse_config_descriptor(const uint8_t *raw, uint16_t len,
                                struct usb_config_descriptor *config);
int usb_parse_interface_descriptor(const uint8_t *raw,
                                   struct usb_interface_descriptor *iface);
int usb_parse_endpoint_descriptor(const uint8_t *raw,
                                  struct usb_endpoint_descriptor *ep);
int usb_print_device_descriptor(const struct usb_device_descriptor *desc);
int usb_print_config_descriptor_full(const struct usb_config_descriptor *config,
                                      const uint8_t *full_config_data,
                                      uint16_t total_length);
int usb_print_interface_descriptor(const struct usb_interface_descriptor *iface);
int usb_print_endpoint_descriptor(const struct usb_endpoint_descriptor *ep);
void usb_update_device_from_desc(struct usb_device *dev,
                                 const struct usb_device_descriptor *desc);

/* ── Configuration sub-descriptor iterator ──────────────────────────── */

/*
 * Callback type for usb_for_each_config_subdesc().
 * Receives each sub-descriptor in a configuration blob.
 * Return 0 to continue iteration, non-zero to stop.
 */
typedef int (*usb_desc_callback_t)(uint8_t bDescriptorType,
                                    const uint8_t *data, uint8_t bLength,
                                    void *user_data);

/*
 * Iterate all sub-descriptors within a configuration descriptor blob.
 * The config descriptor header is skipped; only sub-descriptors
 * (interfaces, endpoints, class-specific, etc.) are passed to the callback.
 * Returns 0 on success, negative errno on error, or the callback's
 * non-zero return value to stop early.
 */
int usb_for_each_config_subdesc(const uint8_t *config_data,
                                 uint16_t total_length,
                                 usb_desc_callback_t callback,
                                 void *user_data);

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
