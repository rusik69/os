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

/* ── USB setup packet (USB 2.0 spec §9.3, 8 bytes) ──────────────────── */
/*
 * Every control transfer begins with an 8-byte setup packet that
 * describes the request type, recipient, and transfer parameters.
 * Packed to match the on-wire format.
 */
struct usb_setup_packet {
    uint8_t  bmRequestType;   /* direction + type + recipient */
    uint8_t  bRequest;        /* request code */
    uint16_t wValue;          /* request-specific value */
    uint16_t wIndex;          /* request-specific index (interface/endpoint) */
    uint16_t wLength;         /* number of bytes in the data stage */
} __attribute__((packed));

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

/* ── USB interface association descriptor (USB-IF IAD, 8 bytes) ───────── */
/*
 * The Interface Association Descriptor (IAD) is defined in the USB-IF
 * IAD ECN and allows multiple interfaces to be grouped as a single
 * function.  Required for USB Video Class (UVC), USB Audio Class (UAC),
 * and other composite devices whose function spans several interfaces.
 * The IAD descriptor must appear before the first interface descriptor
 * it groups within a configuration descriptor.
 *
 * USB-IF IAD ECN: https://www.usb.org/document-library/interface-association-descriptor-ecn
 */
struct usb_iad_descriptor {
    uint8_t  bLength;            /* 8 bytes */
    uint8_t  bDescriptorType;    /* USB_DT_INTERFACE_ASSOC (11) */
    uint8_t  bFirstInterface;    /* index of the first interface in the group */
    uint8_t  bInterfaceCount;    /* number of contiguous interfaces in the group */
    uint8_t  bFunctionClass;     /* class code of the grouped function */
    uint8_t  bFunctionSubClass;  /* subclass code of the grouped function */
    uint8_t  bFunctionProtocol;  /* protocol code of the grouped function */
    uint8_t  iFunction;          /* index to string descriptor describing the function */
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
#define USB_CLASS_VIDEO      0x0E   /* USB Video class (UVC 1.0/1.1/1.5) */

#define USB_MAX_DEVICES 16

int  usb_init(void);              /* initialise all USB host controllers */
void usb_exit(void);              /* shutdown all USB host controllers */
int  usb_is_present(void);
int  usb_get_device_count(void);
struct usb_device *usb_get_device(int idx);

/* ── Descriptor parsing API ──────────────────────────────────────────── */
int usb_parse_device_descriptor(const uint8_t *raw, uint16_t len,
                                struct usb_device_descriptor *desc);
int usb_parse_config_descriptor(const uint8_t *raw, uint16_t len,
                                struct usb_config_descriptor *config);
int usb_parse_interface_descriptor(const uint8_t *raw,
                                   struct usb_interface_descriptor *iface);
int usb_parse_endpoint_descriptor(const uint8_t *raw,
                                  struct usb_endpoint_descriptor *ep);
int usb_parse_iad_descriptor(const uint8_t *raw,
                             struct usb_iad_descriptor *iad);
int usb_print_device_descriptor(const struct usb_device_descriptor *desc);
int usb_print_config_descriptor_full(const struct usb_config_descriptor *config,
                                      const uint8_t *full_config_data,
                                      uint16_t total_length);
int usb_print_interface_descriptor(const struct usb_interface_descriptor *iface);
int usb_print_endpoint_descriptor(const struct usb_endpoint_descriptor *ep);
int usb_print_iad_descriptor(const struct usb_iad_descriptor *iad);
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

/* ── USB power management API ────────────────────────────────────────── */

/*
 * Suspend a USB device. Sends a SET_FEATURE(PORT_SUSPEND) control
 * request and suspends the host controller port.
 * Returns 0 on success, negative errno on failure.
 */
int usb_suspend_device(uint8_t dev_addr);

/*
 * Resume a USB device. Drives resume signalling on the port
 * and restores the device to active state.
 * Returns 0 on success, negative errno on failure.
 */
int usb_resume_device(uint8_t dev_addr);

/*
 * Enable or disable remote wakeup for a USB device.
 * @dev_addr:  USB device address
 * @enable:    1 to enable remote wakeup, 0 to disable
 * Returns 0 on success, negative errno on failure.
 */
int usb_enable_remote_wakeup(uint8_t dev_addr, int enable);

/*
 * Suspend a specific port on the USB host controller.
 * @port:  Port number (0-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_port_suspend(int port);

/*
 * Resume a specific port on the USB host controller.
 * @port:  Port number (0-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_port_resume(int port);

/*
 * Suspend all USB ports and devices.
 * Returns 0 on success, negative errno on failure.
 */
int usb_suspend_all(void);

/*
 * Resume all suspended USB ports and devices.
 * Returns 0 on success, negative errno on failure.
 */
int usb_resume_all(void);

/* EHCI port power management (available for HC-level control) */
int ehci_port_suspend(int ctrl_idx, int port);
int ehci_port_resume(int ctrl_idx, int port);
int ehci_port_reset_resume(int ctrl_idx, int port);
void ehci_suspend_all_ports(void);
void ehci_resume_all_ports(void);
int ehci_port_has_remote_wakeup(int ctrl_idx, int port);

/* ── USB alternate setting selection API ──────────────────────────────────── */

/*
 * USB 2.0 spec §9.4.10: SET_INTERFACE.
 * Each interface on a USB device can have multiple alternate settings
 * (e.g., different endpoint configurations for the same logical function).
 * Alternate setting 0 is the default.
 */

#define USB_MAX_INTERFACES          16      /* maximum interfaces per device */
#define USB_DEFAULT_ALT_SETTING     0       /* default alternate setting */
#define USB_MAX_IADS                8       /* maximum IADs per device */

/*
 * Select an alternate setting for a USB interface.
 * Sends a USB_REQ_SET_INTERFACE standard control request to the device.
 * After this call, the specified interface's endpoints are reconfigured
 * according to the selected alternate setting's descriptor.
 *
 * @dev_addr:    USB device address
 * @iface_num:   Interface number (0-based)
 * @alt_setting: Alternate setting to activate (0 = default)
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_set_interface(uint8_t dev_addr, uint8_t iface_num, uint8_t alt_setting);

/*
 * Get the current alternate setting for a USB interface.
 * Sends a USB_REQ_GET_INTERFACE standard control request to the device.
 *
 * @dev_addr:    USB device address
 * @iface_num:   Interface number (0-based)
 *
 * Returns the current alternate setting number (>= 0) on success,
 * or negative errno on failure.
 */
int usb_get_interface(uint8_t dev_addr, uint8_t iface_num);

/*
 * Record the number of interfaces for a device (used during
 * configuration descriptor parsing / device enumeration).
 * Initialises all interface alternate settings to USB_DEFAULT_ALT_SETTING.
 *
 * @dev_addr:      USB device address
 * @num_interfaces: Number of interfaces on this device
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_set_device_interface_count(uint8_t dev_addr, uint8_t num_interfaces);

/* ── Interface Association Descriptor (IAD) API ───────────────────────── */

/*
 * Maximum number of IADs that can be stored per device.
 * USB-IF IAD ECN: IADs group multiple interfaces into a single function.
 */

/*
 * Parse all Interface Association Descriptors from a configuration
 * descriptor blob and store them in the device's IAD table.
 *
 * @dev_addr:       USB device address
 * @config_data:    Full configuration descriptor blob (including header)
 * @total_length:   Total length of the configuration blob (wTotalLength)
 *
 * Returns the number of IADs stored on success (>= 0),
 * or negative errno on failure.
 */
int usb_parse_iads_from_config(uint8_t dev_addr,
                                const uint8_t *config_data,
                                uint16_t total_length);

/*
 * Look up the Interface Association Descriptor that contains a given
 * interface number.
 *
 * @dev_addr:       USB device address
 * @iface_num:      Interface number to look up
 * @out_iad:        Output pointer for the matching IAD (if found)
 *
 * Returns 0 on success with @out_iad filled, or -ENOENT if no IAD
 * contains the given interface number.
 */
int usb_get_iad_for_interface(uint8_t dev_addr, uint8_t iface_num,
                               struct usb_iad_descriptor *out_iad);

/*
 * Get the number of IADs stored for a USB device.
 *
 * @dev_addr:       USB device address
 *
 * Returns the IAD count (>= 0) on success, or negative errno on failure.
 */
int usb_get_iad_count(uint8_t dev_addr);

/*
 * Get a specific IAD by index for a USB device.
 *
 * @dev_addr:       USB device address
 * @index:          IAD index (0-based)
 * @out_iad:        Output pointer for the IAD at the given index
 *
 * Returns 0 on success with @out_iad filled, or negative errno on failure.
 */
int usb_get_iad(uint8_t dev_addr, int index,
                struct usb_iad_descriptor *out_iad);

#endif
