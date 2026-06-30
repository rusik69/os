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

#include "usb.h"
#include "usb_core.h"
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
int usb_init(void)
{
    kprintf("[USB] Initialising USB subsystem...\n");

    /* Initialise the USB core device model */
    usb_core_init();

    /* Try to initialise EHCI controllers */
    /* ehci_pci_probe_all stub (EHCI not implemented) */
    (void)0;
    int ret = 0;
    if (ret == 0) {
        kprintf("[USB] USB subsystem initialised\n");
    } else {
        kprintf("[USB] No EHCI controllers found\n");
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

int usb_parse_device_descriptor(const uint8_t *raw,
                                struct usb_device_descriptor *desc)
{
    if (!raw || !desc)
        return -EINVAL;
    if (raw[0] < 18)
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
