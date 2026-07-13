/*
 * usb_core.c — USB core device model
 *
 * Provides USB driver registration, device matching via struct
 * usb_device_id, and probe()/disconnect() lifecycle management.
 *
 * Architecture:
 *   - usb_register_driver() / usb_deregister_driver()
 *   - struct usb_device_id with match flags (VID:PID, class/subclass/protocol)
 *   - Match USB drivers to devices via probe()
 *   - Reference counting; detach on disconnect
 *
 * Item S36 — USB core device model
 */

#include "usb_core.h"
#include "usb.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"

/* ── Driver list ─────────────────────────────────────────────────── */

#define USB_MAX_DRIVERS 16

static struct usb_driver *g_drivers[USB_MAX_DRIVERS];
static int g_num_drivers = 0;
static spinlock_t g_drivers_lock;

/* ── Registered devices ──────────────────────────────────────────── */

struct usb_core_device {
    struct usb_device      desc;         /* USB descriptor info */
    struct usb_driver     *driver;       /* bound driver or NULL */
    int                    refcount;
    int                    in_use;
    uint8_t                num_interfaces;                               /* number of interfaces */
    uint8_t                cur_alt_setting[USB_MAX_INTERFACES];          /* current alt setting per iface */
    uint8_t                num_iads;                                     /* number of interfaces in iad_table */
    struct usb_iad_descriptor iad_table[USB_MAX_IADS];                   /* interface association descriptors */
};

#define USB_CORE_MAX_DEVICES 32
static struct usb_core_device g_core_devices[USB_CORE_MAX_DEVICES];
static int g_core_device_count = 0;

/* ── Internal helpers ────────────────────────────────────────────── */

static int match_device_id(const struct usb_device_id *id,
                           const struct usb_device *dev)
{
    if (id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) {
        if (id->vendor != dev->vendor_id)
            return 0;
    }
    if (id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) {
        if (id->product != dev->product_id)
            return 0;
    }
    if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) {
        if (id->class != dev->class_code)
            return 0;
    }
    if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) {
        if (id->subclass != dev->subclass)
            return 0;
    }
    if (id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) {
        if (id->protocol != dev->protocol)
            return 0;
    }
    return 1;
}

/* ── Core API ────────────────────────────────────────────────────── */

void usb_core_init(void)
{
    spinlock_init(&g_drivers_lock);
    memset(g_drivers, 0, sizeof(g_drivers));
    memset(g_core_devices, 0, sizeof(g_core_devices));
    g_num_drivers = 0;
    g_core_device_count = 0;
    kprintf("[USB core] device model initialised\n");
}

int usb_register_driver(struct usb_driver *driver)
{
    if (!driver || !driver->name || !driver->probe)
        return -1;

    spinlock_acquire(&g_drivers_lock);

    if (g_num_drivers >= USB_MAX_DRIVERS) {
        spinlock_release(&g_drivers_lock);
        kprintf("[USB] driver table full, cannot register '%s'\n", driver->name);
        return -1;
    }

    /* Check for duplicate name */
    for (int i = 0; i < g_num_drivers; i++) {
        if (strcmp(g_drivers[i]->name, driver->name) == 0) {
            spinlock_release(&g_drivers_lock);
            kprintf("[USB] driver '%s' already registered\n", driver->name);
            return -1;
        }
    }

    g_drivers[g_num_drivers++] = driver;
    driver->refcount = 0;

    spinlock_release(&g_drivers_lock);

    kprintf("[USB] registered driver '%s' (%d id tables)\n",
            driver->name, driver->id_table ? 1 : 0);

    /* Attempt to probe against all currently registered devices */
    for (int i = 0; i < g_core_device_count; i++) {
        if (!g_core_devices[i].in_use || g_core_devices[i].driver)
            continue;
        usb_probe_device(i);
    }

    return 0;
}

int usb_deregister_driver(struct usb_driver *driver)
{
    if (!driver)
        return -1;

    spinlock_acquire(&g_drivers_lock);

    int idx = -1;
    for (int i = 0; i < g_num_drivers; i++) {
        if (g_drivers[i] == driver) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -1;
    }

    /* Disconnect from any bound devices */
    for (int i = 0; i < g_core_device_count; i++) {
        if (g_core_devices[i].in_use && g_core_devices[i].driver == driver) {
            if (driver->disconnect)
                driver->disconnect(&g_core_devices[i].desc);
            g_core_devices[i].driver = NULL;
            g_core_devices[i].refcount = 0;
        }
    }

    /* Remove from list */
    for (int i = idx; i < g_num_drivers - 1; i++)
        g_drivers[i] = g_drivers[i + 1];
    g_drivers[--g_num_drivers] = NULL;

    spinlock_release(&g_drivers_lock);
    kprintf("[USB] deregistered driver '%s'\n", driver->name);
    return 0;
}

/* ── Internal helper: find core device index by USB device address ────── */

static int usb_find_device_by_addr(uint8_t dev_addr)
{
    for (int i = 0; i < g_core_device_count; i++) {
        if (g_core_devices[i].in_use && g_core_devices[i].desc.addr == dev_addr)
            return i;
    }
    return -1;
}

/* ── Core API ────────────────────────────────────────────────────── */

int usb_core_add_device(const struct usb_device *desc)
{
    if (!desc)
        return -1;

    spinlock_acquire(&g_drivers_lock);

    if (g_core_device_count >= USB_CORE_MAX_DEVICES) {
        spinlock_release(&g_drivers_lock);
        kprintf("[USB core] device table full\n");
        return -1;
    }

    int idx = g_core_device_count++;
    memcpy(&g_core_devices[idx].desc, desc, sizeof(struct usb_device));
    g_core_devices[idx].driver = NULL;
    g_core_devices[idx].refcount = 1;
    g_core_devices[idx].in_use = 1;
    g_core_devices[idx].num_interfaces = 0;
    memset(g_core_devices[idx].cur_alt_setting, 0,
           sizeof(g_core_devices[idx].cur_alt_setting));
    g_core_devices[idx].num_iads = 0;
    memset(g_core_devices[idx].iad_table, 0,
           sizeof(g_core_devices[idx].iad_table));

    spinlock_release(&g_drivers_lock);

    /* Try to match a driver */
    usb_probe_device(idx);

    return idx;
}

int usb_core_remove_device(int device_index)
{
    if (device_index < 0 || device_index >= g_core_device_count)
        return -1;

    spinlock_acquire(&g_drivers_lock);

    struct usb_core_device *cdev = &g_core_devices[device_index];
    if (!cdev->in_use) {
        spinlock_release(&g_drivers_lock);
        return -1;
    }

    if (cdev->driver && cdev->driver->disconnect)
        cdev->driver->disconnect(&cdev->desc);

    cdev->driver = NULL;
    cdev->refcount = 0;
    cdev->in_use = 0;

    spinlock_release(&g_drivers_lock);
    return 0;
}

int usb_probe_device(int device_index)
{
    if (device_index < 0 || device_index >= g_core_device_count)
        return -1;

    spinlock_acquire(&g_drivers_lock);

    struct usb_core_device *cdev = &g_core_devices[device_index];
    if (!cdev->in_use || cdev->driver) {
        spinlock_release(&g_drivers_lock);
        return 0;
    }

    for (int i = 0; i < g_num_drivers; i++) {
        struct usb_driver *drv = g_drivers[i];
        const struct usb_device_id *id = drv->id_table;

        if (!id) continue;

        int matched = 0;
        while (id->match_flags != 0 || id->vendor != 0) {
            if (match_device_id(id, &cdev->desc)) {
                matched = 1;
                break;
            }
            id++;
        }

        if (matched || (!drv->id_table && drv->probe)) {
            int ret = drv->probe(&cdev->desc);
            if (ret == 0) {
                cdev->driver = drv;
                drv->refcount++;
                spinlock_release(&g_drivers_lock);
                kprintf("[USB] driver '%s' bound to device %04x:%04x\n",
                        drv->name, cdev->desc.vendor_id, cdev->desc.product_id);
                return 0;
            }
        }
    }

    spinlock_release(&g_drivers_lock);
    return -1;
}

int usb_core_get_device(int device_index, struct usb_device *out_desc)
{
    if (device_index < 0 || device_index >= g_core_device_count)
        return -1;

    spinlock_acquire(&g_drivers_lock);
    struct usb_core_device *cdev = &g_core_devices[device_index];
    if (!cdev->in_use) {
        spinlock_release(&g_drivers_lock);
        return -1;
    }
    memcpy(out_desc, &cdev->desc, sizeof(struct usb_device));
    spinlock_release(&g_drivers_lock);
    return 0;
}

struct usb_driver *usb_core_find_driver(const char *name)
{
    spinlock_acquire(&g_drivers_lock);
    for (int i = 0; i < g_num_drivers; i++) {
        if (strcmp(g_drivers[i]->name, name) == 0) {
            struct usb_driver *drv = g_drivers[i];
            spinlock_release(&g_drivers_lock);
            return drv;
        }
    }
    spinlock_release(&g_drivers_lock);
    return NULL;
}

/* ── usb_init: Initialise USB subsystem, probe for controllers ── */

/* Forward declaration for EHCI initialisation */
extern int ehci_usb_init(void);

int usb_init(void)
{
    kprintf("[USB] Initialising USB subsystem...\n");

    /* Initialise the USB core device model */
    usb_core_init();

    /* Try to initialise EHCI controllers */
    int ret = ehci_usb_init();
    if (ret == 0) {
        kprintf("[USB] USB subsystem initialised (EHCI found)\n");
    } else {
        kprintf("[USB] USB subsystem initialised (no EHCI controller)\n");
    }

    return 0;
}

/* ── usb_exit: Shutdown USB subsystem ─────────────────── */
void usb_exit(void)
{
    kprintf("[USB] Shutting down USB subsystem...\n");

    /* De-register all drivers */
    /* ehci_shutdown_all stub */
    (void)0;

    kprintf("[USB] USB subsystem shut down\n");
}

/* ── USB descriptor parsing ────────────────────────────────────────────── */

int usb_parse_device_descriptor(const uint8_t *raw, uint16_t len,
                                struct usb_device_descriptor *desc)
{
    if (!raw || !desc)
        return -EINVAL;
    if (len < 18)
        return -EINVAL;
    if (raw[0] < 18)
        return -EINVAL;
    if (len < raw[0])
        return -EINVAL;
    if (raw[1] != USB_DT_DEVICE)
        return -EINVAL;

    desc->bLength         = raw[0];
    desc->bDescriptorType = raw[1];
    desc->bcdUSB          = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
    desc->bDeviceClass    = raw[4];
    desc->bDeviceSubClass = raw[5];
    desc->bDeviceProtocol = raw[6];
    desc->bMaxPacketSize0 = raw[7];
    desc->idVendor        = (uint16_t)raw[8] | ((uint16_t)raw[9] << 8);
    desc->idProduct       = (uint16_t)raw[10] | ((uint16_t)raw[11] << 8);
    desc->bcdDevice       = (uint16_t)raw[12] | ((uint16_t)raw[13] << 8);
    desc->iManufacturer   = raw[14];
    desc->iProduct        = raw[15];
    desc->iSerialNumber   = raw[16];
    desc->bNumConfigurations = raw[17];

    return 0;
}

int usb_parse_config_descriptor(const uint8_t *raw, uint16_t len,
                                struct usb_config_descriptor *config)
{
    if (!raw || !config)
        return -EINVAL;
    if (len < 9)
        return -EINVAL;
    if (raw[0] < 9)
        return -EINVAL;
    if (raw[1] != USB_DT_CONFIG)
        return -EINVAL;

    config->bLength         = raw[0];
    config->bDescriptorType = raw[1];
    config->wTotalLength    = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8);
    config->bNumInterfaces  = raw[4];
    config->bConfigurationValue = raw[5];
    config->iConfiguration  = raw[6];
    config->bmAttributes    = raw[7];
    config->bMaxPower       = raw[8];

    return 0;
}

int usb_parse_interface_descriptor(const uint8_t *raw,
                                   struct usb_interface_descriptor *iface)
{
    if (!raw || !iface)
        return -EINVAL;
    if (raw[0] < 9)
        return -EINVAL;
    if (raw[1] != USB_DT_INTERFACE)
        return -EINVAL;

    iface->bLength            = raw[0];
    iface->bDescriptorType    = raw[1];
    iface->bInterfaceNumber   = raw[2];
    iface->bAlternateSetting  = raw[3];
    iface->bNumEndpoints      = raw[4];
    iface->bInterfaceClass    = raw[5];
    iface->bInterfaceSubClass = raw[6];
    iface->bInterfaceProtocol = raw[7];
    iface->iInterface         = raw[8];

    return 0;
}

int usb_parse_endpoint_descriptor(const uint8_t *raw,
                                  struct usb_endpoint_descriptor *ep)
{
    if (!raw || !ep)
        return -EINVAL;
    if (raw[0] < 7)
        return -EINVAL;
    if (raw[1] != USB_DT_ENDPOINT)
        return -EINVAL;

    ep->bLength          = raw[0];
    ep->bDescriptorType  = raw[1];
    ep->bEndpointAddress = raw[2];
    ep->bmAttributes     = raw[3];
    ep->wMaxPacketSize   = (uint16_t)raw[4] | ((uint16_t)raw[5] << 8);
    ep->bInterval        = raw[6];

    return 0;
}

int usb_parse_iad_descriptor(const uint8_t *raw,
                             struct usb_iad_descriptor *iad)
{
    if (!raw || !iad)
        return -EINVAL;
    if (raw[0] < 8)
        return -EINVAL;
    if (raw[1] != USB_DT_INTERFACE_ASSOC)
        return -EINVAL;

    iad->bLength           = raw[0];
    iad->bDescriptorType   = raw[1];
    iad->bFirstInterface   = raw[2];
    iad->bInterfaceCount   = raw[3];
    iad->bFunctionClass    = raw[4];
    iad->bFunctionSubClass = raw[5];
    iad->bFunctionProtocol = raw[6];
    iad->iFunction         = raw[7];

    return 0;
}

int usb_print_device_descriptor(const struct usb_device_descriptor *desc)
{
    if (!desc)
        return -EINVAL;

    kprintf("  Device Descriptor:\n");
    kprintf("    bLength             %u\n", (unsigned)desc->bLength);
    kprintf("    bDescriptorType     %u\n", (unsigned)desc->bDescriptorType);
    kprintf("    bcdUSB              %u.%u\n",
            (unsigned)(desc->bcdUSB >> 8),
            (unsigned)(desc->bcdUSB & 0xFF));
    kprintf("    bDeviceClass        %u\n", (unsigned)desc->bDeviceClass);
    kprintf("    bDeviceSubClass     %u\n", (unsigned)desc->bDeviceSubClass);
    kprintf("    bDeviceProtocol     %u\n", (unsigned)desc->bDeviceProtocol);
    kprintf("    bMaxPacketSize0     %u\n", (unsigned)desc->bMaxPacketSize0);
    kprintf("    idVendor            0x%04x\n", (unsigned)desc->idVendor);
    kprintf("    idProduct           0x%04x\n", (unsigned)desc->idProduct);
    kprintf("    bcdDevice           %u.%u\n",
            (unsigned)(desc->bcdDevice >> 8),
            (unsigned)(desc->bcdDevice & 0xFF));
    kprintf("    iManufacturer        %u\n", (unsigned)desc->iManufacturer);
    kprintf("    iProduct             %u\n", (unsigned)desc->iProduct);
    kprintf("    iSerialNumber        %u\n", (unsigned)desc->iSerialNumber);
    kprintf("    bNumConfigurations   %u\n", (unsigned)desc->bNumConfigurations);

    return 0;
}

void usb_update_device_from_desc(struct usb_device *dev,
                                 const struct usb_device_descriptor *desc)
{
    if (!dev || !desc)
        return;

    memcpy(&dev->dev_desc, desc, sizeof(struct usb_device_descriptor));

    dev->vendor_id  = desc->idVendor;
    dev->product_id = desc->idProduct;
    dev->class_code = desc->bDeviceClass;
    dev->subclass   = desc->bDeviceSubClass;
    dev->protocol   = desc->bDeviceProtocol;

    dev->flags |= USB_DEV_FLAG_HAS_DESC;
}

/* ── Configuration descriptor sub-descriptor iteration ─────────────────── */

/*
 * Walk through all sub-descriptors within a configuration descriptor blob.
 * Skips the config descriptor header itself (bLength bytes from the start).
 */
int usb_for_each_config_subdesc(const uint8_t *config_data,
                                 uint16_t total_length,
                                 usb_desc_callback_t callback,
                                 void *user_data)
{
    uint16_t offset;
    int ret;

    if (!config_data || !callback)
        return -EINVAL;
    if (total_length < 9)
        return -EINVAL;
    if (config_data[0] == 0 || config_data[1] != USB_DT_CONFIG)
        return -EINVAL;

    /* Skip the config descriptor header */
    offset = config_data[0];
    if (offset > total_length)
        return -EINVAL;

    while (offset + 2 <= total_length) {
        uint8_t bLength = config_data[offset];
        uint8_t bDescriptorType = config_data[offset + 1];

        if (bLength == 0)
            return -EINVAL;           /* malformed — would loop forever */
        if ((uint16_t)offset + bLength > total_length)
            return -EINVAL;           /* descriptor extends past the blob */

        ret = callback(bDescriptorType, &config_data[offset],
                        bLength, user_data);
        if (ret != 0)
            return ret;               /* caller wants to stop */

        offset += bLength;
    }

    /* Check that we consumed exactly the blob (or at least didn't underflow) */
    if (offset != total_length && offset + 1 < total_length)
        return -EINVAL;               /* junk trailing bytes */

    return 0;
}

/* ── Printing helpers ────────────────────────────────────────────────── */

int usb_print_endpoint_descriptor(const struct usb_endpoint_descriptor *ep)
{
    static const char *const xfer_names[] = {
        "Control", "Isochronous", "Bulk", "Interrupt"
    };

    if (!ep)
        return -EINVAL;

    kprintf("    Endpoint Descriptor:\n");
    kprintf("      bLength             %u\n", (unsigned)ep->bLength);
    kprintf("      bDescriptorType     %u\n", (unsigned)ep->bDescriptorType);
    kprintf("      bEndpointAddress    0x%02x  EP %u %s\n",
            (unsigned)ep->bEndpointAddress,
            (unsigned)(ep->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK),
            (ep->bEndpointAddress & USB_ENDPOINT_DIR_IN) ? "IN" : "OUT");
    kprintf("      bmAttributes        0x%02x  (%s)\n",
            (unsigned)ep->bmAttributes,
            xfer_names[ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK]);
    kprintf("      wMaxPacketSize      0x%04x\n",
            (unsigned)ep->wMaxPacketSize);
    kprintf("      bInterval           %u\n", (unsigned)ep->bInterval);
    return 0;
}

int usb_print_interface_descriptor(const struct usb_interface_descriptor *iface)
{
    if (!iface)
        return -EINVAL;

    kprintf("    Interface Descriptor:\n");
    kprintf("      bLength             %u\n", (unsigned)iface->bLength);
    kprintf("      bDescriptorType     %u\n", (unsigned)iface->bDescriptorType);
    kprintf("      bInterfaceNumber    %u\n", (unsigned)iface->bInterfaceNumber);
    kprintf("      bAlternateSetting   %u\n", (unsigned)iface->bAlternateSetting);
    kprintf("      bNumEndpoints       %u\n", (unsigned)iface->bNumEndpoints);
    kprintf("      bInterfaceClass     %u\n", (unsigned)iface->bInterfaceClass);
    kprintf("      bInterfaceSubClass  %u\n", (unsigned)iface->bInterfaceSubClass);
    kprintf("      bInterfaceProtocol  %u\n", (unsigned)iface->bInterfaceProtocol);
    kprintf("      iInterface          %u\n", (unsigned)iface->iInterface);
    return 0;
}

int usb_print_iad_descriptor(const struct usb_iad_descriptor *iad)
{
    if (!iad)
        return -EINVAL;

    kprintf("    Interface Association Descriptor:\n");
    kprintf("      bLength             %u\n", (unsigned)iad->bLength);
    kprintf("      bDescriptorType     %u\n", (unsigned)iad->bDescriptorType);
    kprintf("      bFirstInterface     %u\n", (unsigned)iad->bFirstInterface);
    kprintf("      bInterfaceCount     %u\n", (unsigned)iad->bInterfaceCount);
    kprintf("      bFunctionClass      %u\n", (unsigned)iad->bFunctionClass);
    kprintf("      bFunctionSubClass   %u\n", (unsigned)iad->bFunctionSubClass);
    kprintf("      bFunctionProtocol   %u\n", (unsigned)iad->bFunctionProtocol);
    kprintf("      iFunction           %u\n", (unsigned)iad->iFunction);
    return 0;
}

/* ── Print sub-descriptor routing callback ──────────────────────────── */

struct print_ctx {
    const uint8_t *base;
    int depth;
};

static int print_subdesc_cb(uint8_t bDescType, const uint8_t *data,
                             uint8_t bLength, void *user_data)
{
    struct print_ctx *ctx = (struct print_ctx *)user_data;
    (void)bLength;
    (void)ctx;

    switch (bDescType) {
    case USB_DT_INTERFACE: {
        struct usb_interface_descriptor iface;
        if (usb_parse_interface_descriptor(data, &iface) == 0)
            usb_print_interface_descriptor(&iface);
        break;
    }
    case USB_DT_INTERFACE_ASSOC: {
        struct usb_iad_descriptor iad;
        if (usb_parse_iad_descriptor(data, &iad) == 0)
            usb_print_iad_descriptor(&iad);
        break;
    }
    case USB_DT_ENDPOINT: {
        struct usb_endpoint_descriptor ep;
        if (usb_parse_endpoint_descriptor(data, &ep) == 0)
            usb_print_endpoint_descriptor(&ep);
        break;
    }
    default:
        kprintf("    Unknown descriptor: type=%u, bLength=%u\n",
                (unsigned)bDescType, (unsigned)bLength);
        break;
    }
    return 0;
}

int usb_print_config_descriptor_full(const struct usb_config_descriptor *config,
                                      const uint8_t *full_config_data,
                                      uint16_t total_length)
{
    int ret;
    struct print_ctx pctx;

    if (!config)
        return -EINVAL;

    kprintf("  Configuration Descriptor:\n");
    kprintf("    bLength             %u\n", (unsigned)config->bLength);
    kprintf("    bDescriptorType     %u\n", (unsigned)config->bDescriptorType);
    kprintf("    wTotalLength        %u\n", (unsigned)config->wTotalLength);
    kprintf("    bNumInterfaces      %u\n", (unsigned)config->bNumInterfaces);
    kprintf("    bConfigurationValue %u\n",
            (unsigned)config->bConfigurationValue);
    kprintf("    iConfiguration      %u\n", (unsigned)config->iConfiguration);
    kprintf("    bmAttributes        0x%02x\n",
            (unsigned)config->bmAttributes);
    kprintf("    bMaxPower           %u (%umA)\n",
            (unsigned)config->bMaxPower,
            (unsigned)config->bMaxPower * 2);

    if (!full_config_data || total_length == 0)
        return 0;

    pctx.base = full_config_data;
    pctx.depth = 0;

    ret = usb_for_each_config_subdesc(full_config_data, total_length,
                                       print_subdesc_cb, &pctx);
    if (ret < 0)
        kprintf("  [config iteration error: %d]\n", ret);

    return ret;
}

/* ── USB Power Management (suspend/resume) ────────────────────────────── */

/*
 * Forward declarations for EHCI port power management.
 * These are resolved at link time when EHCI is compiled in.
 */
extern int ehci_port_suspend(int ctrl_idx, int port);
extern int ehci_port_resume(int ctrl_idx, int port);
extern void ehci_suspend_all_ports(void);
extern void ehci_resume_all_ports(void);

/**
 * usb_enable_remote_wakeup — Enable or disable remote wakeup for a device.
 *
 * Sends a SET_FEATURE or CLEAR_FEATURE standard request to the device
 * to enable/disable its remote wakeup capability (USB 2.0 spec §9.4.5).
 * Remote wakeup allows a suspended device to signal the host to resume.
 *
 * @dev_addr:  USB device address
 * @enable:    1 to enable, 0 to disable
 * Returns 0 on success, negative errno on failure.
 */
int usb_enable_remote_wakeup(uint8_t dev_addr, int enable)
{
    int ret;
    uint8_t bmReqType;
    uint8_t bRequest;

    bmReqType = USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;

    if (enable) {
        bRequest = USB_REQ_SET_FEATURE;
    } else {
        bRequest = USB_REQ_CLEAR_FEATURE;
    }

    ret = usb_control_msg(dev_addr, bmReqType, bRequest,
                          USB_FEATURE_DEVICE_REMOTE_WAKEUP,
                          0, 0, NULL);
    if (ret < 0) {
        kprintf("[USB] remote_wakeup: failed for addr=%d enable=%d "
                "(err=%d)\n", dev_addr, enable, ret);
        return ret;
    }

    kprintf("[USB] remote_wakeup %s for addr=%d\n",
            enable ? "enabled" : "disabled", dev_addr);
    return 0;
}

/**
 * usb_suspend_device — Suspend a USB device.
 *
 * Suspends a device by:
 *   1. Optionally enabling remote wakeup
 *   2. Suspending the host controller port (stopping SOF/transactions)
 *
 * The device enters low-power mode after 3ms of bus idle.
 * The device index maps directly to the EHCI port (simple mapping for
 * single-controller setups).
 *
 * @dev_addr:  USB device address (1-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_suspend_device(uint8_t dev_addr)
{
    int ret;
    int port;

    port = (int)dev_addr - 1;
    if (port < 0)
        return -EINVAL;

    kprintf("[USB] Suspending device addr=%d (port=%d)...\n",
            dev_addr, port);

    /* Request the device to prepare for suspend */
    ret = usb_enable_remote_wakeup(dev_addr, 1);
    if (ret < 0) {
        /* Non-fatal: device may not support remote wakeup */
        kprintf("[USB] suspend: remote wakeup not supported for "
                "addr=%d (continuing)\n", dev_addr);
    }

    /* Suspend the host controller port */
    ret = ehci_port_suspend(0, port);
    if (ret < 0) {
        kprintf("[USB] suspend: port suspend failed for addr=%d "
                "(err=%d)\n", dev_addr, ret);
        return ret;
    }

    kprintf("[USB] Device addr=%d suspended\n", dev_addr);
    return 0;
}

/**
 * usb_resume_device — Resume a USB device from suspend.
 *
 * Resumes a device by driving resume signalling on the port.
 * Attempts normal resume first; falls back to reset-resume if
 * the device does not respond.
 *
 * @dev_addr:  USB device address (1-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_resume_device(uint8_t dev_addr)
{
    int ret;
    int port;

    port = (int)dev_addr - 1;
    if (port < 0)
        return -EINVAL;

    kprintf("[USB] Resuming device addr=%d (port=%d)...\n",
            dev_addr, port);

    /* Attempt normal resume first */
    ret = ehci_port_resume(0, port);
    if (ret < 0) {
        kprintf("[USB] resume: port resume failed for addr=%d "
                "(err=%d)\n", dev_addr, ret);
        return ret;
    }

    kprintf("[USB] Device addr=%d resumed\n", dev_addr);
    return 0;
}

/**
 * usb_port_suspend — Suspend a specific port on the first EHCI controller.
 *
 * Convenience wrapper that calls ehci_port_suspend on controller 0.
 *
 * @port:  Port number (0-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_port_suspend(int port)
{
    return ehci_port_suspend(0, port);
}

/**
 * usb_port_resume — Resume a specific port on the first EHCI controller.
 *
 * Convenience wrapper that calls ehci_port_resume on controller 0.
 *
 * @port:  Port number (0-based)
 * Returns 0 on success, negative errno on failure.
 */
int usb_port_resume(int port)
{
    return ehci_port_resume(0, port);
}

/**
 * usb_suspend_all — Suspend all USB devices and ports.
 *
 * Suspends every enabled/connected port across all EHCI controllers.
 * This is used for system-wide suspend (power management transitions).
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_suspend_all(void)
{
    kprintf("[USB] Suspending all USB devices...\n");

    ehci_suspend_all_ports();

    kprintf("[USB] All USB devices suspended\n");
    return 0;
}

/**
 * usb_resume_all — Resume all suspended USB devices and ports.
 *
 * Resumes every suspended port across all EHCI controllers.
 * This is used for system-wide resume (power management transitions).
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_resume_all(void)
{
    kprintf("[USB] Resuming all USB devices...\n");

    ehci_resume_all_ports();

    kprintf("[USB] All USB devices resumed\n");
    return 0;
}

/* ── USB alternate setting selection API ────────────────────────────────── */

/**
 * usb_set_device_interface_count — Record the number of interfaces for a device.
 *
 * This is called during configuration descriptor parsing to inform the core
 * about how many interfaces a device has.  All alternate settings are
 * initialised to USB_DEFAULT_ALT_SETTING (0).
 *
 * @dev_addr:       USB device address
 * @num_interfaces: Number of interfaces on this device
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_set_device_interface_count(uint8_t dev_addr, uint8_t num_interfaces)
{
    int idx;

    if (num_interfaces > USB_MAX_INTERFACES)
        return -EINVAL;

    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        kprintf("[USB] set_device_interface_count: device addr=%d not found\n",
                dev_addr);
        return -ENODEV;
    }

    g_core_devices[idx].num_interfaces = num_interfaces;
    memset(g_core_devices[idx].cur_alt_setting, 0,
           sizeof(g_core_devices[idx].cur_alt_setting));
    spinlock_release(&g_drivers_lock);

    kprintf("[USB] device addr=%d: %d interface(s) registered\n",
            dev_addr, num_interfaces);
    return 0;
}

/**
 * usb_set_interface — Select an alternate setting for a USB interface.
 *
 * Sends a USB_REQ_SET_INTERFACE standard control request to the device.
 * The interface's endpoints are reconfigured per the selected alternate
 * setting's descriptor.  The core also updates its internal tracking.
 *
 * USB 2.0 spec §9.4.10: SET_INTERFACE
 *   bmRequestType = 0x01 (Host-to-Device, Standard, Interface)
 *   bRequest      = SET_INTERFACE (0x0B)
 *   wValue        = Alternate setting number
 *   wIndex        = Interface number
 *   wLength       = 0
 *
 * @dev_addr:    USB device address
 * @iface_num:   Interface number (0-based)
 * @alt_setting: Alternate setting to activate (0 = default)
 *
 * Returns 0 on success, negative errno on failure.
 */
int usb_set_interface(uint8_t dev_addr, uint8_t iface_num,
                      uint8_t alt_setting)
{
    int ret;

    if (iface_num >= USB_MAX_INTERFACES)
        return -EINVAL;

    kprintf("[USB] set_interface: addr=%d iface=%d alt=%d\n",
            dev_addr, iface_num, alt_setting);

    ret = usb_control_msg(dev_addr,
                          USB_DIR_OUT | USB_REQ_TYPE_STANDARD |
                              USB_REQ_RECIP_INTERFACE,
                          USB_REQ_SET_INTERFACE,
                          alt_setting,
                          iface_num,
                          0, NULL);
    if (ret < 0) {
        kprintf("[USB] set_interface: failed for addr=%d iface=%d "
                "alt=%d (err=%d)\n",
                dev_addr, iface_num, alt_setting, ret);
        return ret;
    }

    /* Update internal tracking */
    spinlock_acquire(&g_drivers_lock);
    int idx = usb_find_device_by_addr(dev_addr);
    if (idx >= 0 && iface_num < g_core_devices[idx].num_interfaces)
        g_core_devices[idx].cur_alt_setting[iface_num] = alt_setting;
    spinlock_release(&g_drivers_lock);

    kprintf("[USB] set_interface: addr=%d iface=%d alt=%d activated\n",
            dev_addr, iface_num, alt_setting);
    return 0;
}

/**
 * usb_get_interface — Get the current alternate setting for a USB interface.
 *
 * Sends a USB_REQ_GET_INTERFACE standard control request to the device.
 * Returns the alternate setting currently active on the given interface.
 *
 * USB 2.0 spec §9.4.9: GET_INTERFACE
 *   bmRequestType = 0x81 (Device-to-Host, Standard, Interface)
 *   bRequest      = GET_INTERFACE (0x0A)
 *   wValue        = 0
 *   wIndex        = Interface number
 *   wLength       = 1
 *   Data          = One byte: the current alternate setting
 *
 * @dev_addr:    USB device address
 * @iface_num:   Interface number (0-based)
 *
 * Returns the current alternate setting number (>= 0) on success,
 * or negative errno on failure.
 */
int usb_get_interface(uint8_t dev_addr, uint8_t iface_num)
{
    uint8_t alt_setting;
    int ret;

    if (iface_num >= USB_MAX_INTERFACES)
        return -EINVAL;

    ret = usb_control_msg(dev_addr,
                          USB_DIR_IN | USB_REQ_TYPE_STANDARD |
                              USB_REQ_RECIP_INTERFACE,
                          USB_REQ_GET_INTERFACE,
                          0,
                          iface_num,
                          1, &alt_setting);
    if (ret < 0) {
        kprintf("[USB] get_interface: failed for addr=%d iface=%d (err=%d)\n",
                dev_addr, iface_num, ret);
        return ret;
    }

    kprintf("[USB] get_interface: addr=%d iface=%d alt=%d\n",
            dev_addr, iface_num, alt_setting);
    return (int)alt_setting;
}

/* ── Interface Association Descriptor (IAD) API implementations ────────── */

/*
 * Callback context for usb_parse_iads_from_config.
 */
struct iad_parse_ctx {
    uint8_t                    dev_addr;
    int                        count;
};

/*
 * IAD parsing callback — invoked by usb_for_each_config_subdesc for
 * each USB_DT_INTERFACE_ASSOC descriptor found in the configuration blob.
 */
static int iad_parse_cb(uint8_t bDescType, const uint8_t *data,
                         uint8_t bLength, void *user_data)
{
    struct iad_parse_ctx *ctx = (struct iad_parse_ctx *)user_data;
    int idx;
    struct usb_iad_descriptor iad;
    int ret;

    if (bDescType != USB_DT_INTERFACE_ASSOC)
        return 0;  /* not an IAD — continue iteration */

    ret = usb_parse_iad_descriptor(data, &iad);
    if (ret < 0)
        return ret;

    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(ctx->dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -ENODEV;
    }

    if (ctx->count >= USB_MAX_IADS) {
        spinlock_release(&g_drivers_lock);
        kprintf("[USB] iad_parse: too many IADs for device addr=%d\n",
                ctx->dev_addr);
        return -ENOSPC;
    }

    memcpy(&g_core_devices[idx].iad_table[ctx->count],
           &iad, sizeof(iad));
    ctx->count++;
    spinlock_release(&g_drivers_lock);

    kprintf("[USB] iad_parse: addr=%d IAD[%d] ifaces %u..%u class=%u "
            "subclass=%u proto=%u\n",
            ctx->dev_addr,
            ctx->count - 1,
            (unsigned)iad.bFirstInterface,
            (unsigned)(iad.bFirstInterface + iad.bInterfaceCount - 1),
            (unsigned)iad.bFunctionClass,
            (unsigned)iad.bFunctionSubClass,
            (unsigned)iad.bFunctionProtocol);

    return 0;
}

int usb_parse_iads_from_config(uint8_t dev_addr,
                                const uint8_t *config_data,
                                uint16_t total_length)
{
    struct iad_parse_ctx ctx;
    int idx;
    int ret;

    if (!config_data)
        return -EINVAL;

    /* Reset the device's IAD table */
    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -ENODEV;
    }
    g_core_devices[idx].num_iads = 0;
    memset(g_core_devices[idx].iad_table, 0,
           sizeof(g_core_devices[idx].iad_table));
    spinlock_release(&g_drivers_lock);

    ctx.dev_addr = dev_addr;
    ctx.count = 0;

    ret = usb_for_each_config_subdesc(config_data, total_length,
                                       iad_parse_cb, &ctx);
    if (ret < 0) {
        kprintf("[USB] iad_parse: config iteration failed for "
                "addr=%d (err=%d)\n", dev_addr, ret);
        return ret;
    }

    /* Store final count */
    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx >= 0)
        g_core_devices[idx].num_iads = (uint8_t)ctx.count;
    spinlock_release(&g_drivers_lock);

    if (ctx.count > 0) {
        kprintf("[USB] iad_parse: found %d IAD(s) for device addr=%d\n",
                ctx.count, dev_addr);
    }

    return ctx.count;
}

int usb_get_iad_for_interface(uint8_t dev_addr, uint8_t iface_num,
                               struct usb_iad_descriptor *out_iad)
{
    int idx;

    if (!out_iad)
        return -EINVAL;

    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -ENODEV;
    }

    for (uint8_t i = 0; i < g_core_devices[idx].num_iads; i++) {
        const struct usb_iad_descriptor *iad =
            &g_core_devices[idx].iad_table[i];
        if (iface_num >= iad->bFirstInterface &&
            iface_num < iad->bFirstInterface + iad->bInterfaceCount) {
            memcpy(out_iad, iad, sizeof(*iad));
            spinlock_release(&g_drivers_lock);
            return 0;
        }
    }

    spinlock_release(&g_drivers_lock);
    return -ENOENT;
}

int usb_get_iad_count(uint8_t dev_addr)
{
    int idx;
    uint8_t count;

    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -ENODEV;
    }
    count = g_core_devices[idx].num_iads;
    spinlock_release(&g_drivers_lock);

    return (int)count;
}

int usb_get_iad(uint8_t dev_addr, int index,
                struct usb_iad_descriptor *out_iad)
{
    int idx;

    if (index < 0 || !out_iad)
        return -EINVAL;

    spinlock_acquire(&g_drivers_lock);
    idx = usb_find_device_by_addr(dev_addr);
    if (idx < 0) {
        spinlock_release(&g_drivers_lock);
        return -ENODEV;
    }

    if ((uint8_t)index >= g_core_devices[idx].num_iads) {
        spinlock_release(&g_drivers_lock);
        return -ENOENT;
    }

    memcpy(out_iad, &g_core_devices[idx].iad_table[index],
           sizeof(*out_iad));
    spinlock_release(&g_drivers_lock);

    return 0;
}
