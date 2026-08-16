/*
 * usb_printer.c — USB Printer Class driver
 *
 * Implements USB printer class (bInterfaceClass=7) support.
 * Provides a /dev/usb/lp0 character device for userspace access.
 * Bulk OUT endpoint for data transfer, Bulk IN endpoint for
 * device status (IEEE 1284 status byte).
 *
 * References:
 *   USB Device Class Definition for Printer Devices, Version 1.2
 *   IEEE 1284-2000 Standard for Signaling and Return
 *
 * Item S45 — USB printer class driver (enhanced)
 */

#define KERNEL_INTERNAL
#include "usb.h"
#include "usb_core.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "pmm.h"
#include "spinlock.h"
#include "errno.h"
#include "devfs.h"
#include "module.h"
#include "types.h"

/* ── USB Printer class constants ────────────────────────────────── */
#define USB_CLASS_PRINTER        0x07
#define USB_PRINTER_SUBCLASS     0x01    /* Printer */
#define USB_PRINTER_PROTO_UNIDIR 0x01    /* Uni-directional */
#define USB_PRINTER_PROTO_BIDIR  0x02    /* Bi-directional */
#define USB_PRINTER_PROTO_1284   0x03    /* IEEE 1284.4 compatible */

/* Printer status bits (IEEE 1284) */
#define PRINTER_STATUS_PAPER_EMPTY  0x20
#define PRINTER_STATUS_SELECTED     0x10
#define PRINTER_STATUS_NO_ERROR     0x08
#define PRINTER_STATUS_BUSY         0x40
#define PRINTER_STATUS_FAULT        0x08   /* inverted: 0 = fault */

/* Printer class-specific requests (USB PDF §7) */
#define PRINTER_REQ_GET_DEVICE_ID   0x00
#define PRINTER_REQ_GET_STATUS      0x01
#define PRINTER_REQ_SOFT_RESET      0x02

/* Class-specific request type for printer interface */
#define PRINTER_REQTYPE_GET         (USB_DIR_IN | USB_REQ_TYPE_CLASS | \
                                     USB_REQ_RECIP_INTERFACE)
#define PRINTER_REQTYPE_SET         (USB_DIR_OUT | USB_REQ_TYPE_CLASS | \
                                     USB_REQ_RECIP_INTERFACE)

/* Default data toggle for bulk endpoints (starts at 0 per USB spec) */
#define PRINTER_BULK_TOGGLE_INIT    0

/* ── Printer device instance ─────────────────────────────────────── */
#define MAX_PRINTERS 4
#define PRINTER_CONFIG_BUF_SIZE    256

struct usb_printer {
	uint8_t  dev_addr;            /* USB device address */
	uint16_t vendor_id;
	uint16_t product_id;

	uint8_t  iface_num;           /* Interface number */

	uint8_t  bulk_out_ep;         /* Bulk OUT endpoint address */
	uint8_t  bulk_in_ep;          /* Bulk IN endpoint address */
	uint8_t  protocol;            /* Printer protocol */

	uint8_t  bulk_out_toggle;     /* Data toggle for OUT endpoint */
	uint8_t  bulk_in_toggle;      /* Data toggle for IN endpoint */

	uint8_t  status;              /* Cached IEEE 1284 status */

	char     device_id[256];      /* IEEE 1284 device ID string */
	int      device_id_len;

	spinlock_t lock;
	int      present;
	int      open_count;
};

static struct usb_printer g_printers[MAX_PRINTERS];
static int g_printer_count = 0;
static int g_initialized = 0;

/* ── Config descriptor parse context ─────────────────────────────── */

struct printer_parse_ctx {
	uint8_t  dev_addr;
	uint16_t vendor_id;
	uint16_t product_id;
	uint8_t  bulk_out_ep;
	uint8_t  bulk_in_ep;
	uint8_t  iface_num;
	uint8_t  protocol;
	int      found_printer;
	int      found_bulk_out;
	int      found_bulk_in;
	int      current_iface_valid;
	uint8_t  current_iface_num;
	uint8_t  current_protocol;
};

/* ── Device ID string parsing ────────────────────────────────────── */

/* Parse IEEE 1284 Device ID (MFG:xxx;MDL:xxx;CMD:xxx;...) */
static void parse_device_id(struct usb_printer *p, const uint8_t *data, int len)
{
	if (!data || len <= 0)
		return;

	/* IEEE 1284 Device ID is typically a length-prefixed string:
	 *   [2 bytes: length (little-endian)] [ASCII key:value; pairs]
	 */
	int id_len = 0;
	if (len >= 2) {
		id_len = (int)data[0] | ((int)data[1] << 8);
		int offset = 2;
		if (id_len > len - 2)
			id_len = len - 2;
		if ((size_t)id_len > sizeof(p->device_id) - 1)
			id_len = (int)sizeof(p->device_id) - 1;
		memcpy(p->device_id, data + offset, (size_t)id_len);
		p->device_id[id_len] = '\0';
	}
	p->device_id_len = id_len;
}

/* ── Class-specific control requests ─────────────────────────────── */

/*
 * Send a GET_DEVICE_ID class-specific request to the printer.
 * The device ID is returned in a data buffer (max 256 bytes).
 * Returns the number of bytes in the device ID on success,
 * negative errno on failure.
 */
static int request_device_id(uint8_t dev_addr, uint8_t iface_num,
			     uint8_t *buf, uint16_t buf_size)
{
	/*
	 * USB Printer Class spec §7:
	 * GET_DEVICE_ID (bRequest=0x00)
	 *   bmRequestType = 0xA1 (IN, Class, Interface)
	 *   wValue = 0x0000
	 *   wIndex = interface number
	 *   wLength = max response size
	 *
	 * The device returns a length-prefixed IEEE 1284 Device ID:
	 *   [2 bytes LE length] [ASCII key:value;...]
	 */
	return usb_control_msg(dev_addr, PRINTER_REQTYPE_GET,
			       PRINTER_REQ_GET_DEVICE_ID,
			       0, iface_num, buf_size, buf);
}

/*
 * Send a GET_STATUS class-specific request to the printer.
 * Returns the IEEE 1284 status byte (> 0) on success,
 * or negative errno on failure.
 */
static int request_status(uint8_t dev_addr, uint8_t iface_num)
{
	/*
	 * USB Printer Class spec §7:
	 * GET_STATUS (bRequest=0x01)
	 *   bmRequestType = 0xA1 (IN, Class, Interface)
	 *   wValue = 0x0000
	 *   wIndex = interface number
	 *   wLength = 1
	 * Returns a single byte: IEEE 1284 status.
	 */
	uint8_t status = 0;
	int ret;

	ret = usb_control_msg(dev_addr, PRINTER_REQTYPE_GET,
			      PRINTER_REQ_GET_STATUS,
			      0, iface_num, 1, &status);
	if (ret < 0)
		return ret;

	return (int)status;
}



/* ── Config descriptor callback ──────────────────────────────────── */

/*
 * Callback for usb_for_each_config_subdesc.
 * Walks configuration sub-descriptors looking for a printer interface
 * (bInterfaceClass = 0x07) and its bulk endpoints.
 */
static int printer_desc_callback(uint8_t bDescriptorType,
				 const uint8_t *data, uint8_t bLength,
				 void *user_data)
{
	struct printer_parse_ctx *ctx =
		(struct printer_parse_ctx *)user_data;

	if (bDescriptorType == USB_DT_INTERFACE) {
		/*
		 * Interface descriptor encountered.
		 * Check if it's a printer class interface.
		 * The subdesc walker only guarantees bLength bytes at data,
		 * so validate the length BEFORE reading any fields: a crafted
		 * short descriptor would otherwise over-read bInterfaceClass
		 * (offset 5) past the end of the config buffer.
		 */
		if (bLength < sizeof(struct usb_interface_descriptor)) {
			ctx->current_iface_valid = 0;
			return 0;
		}

		const struct usb_interface_descriptor *iface =
			(const struct usb_interface_descriptor *)data;

		if (iface->bInterfaceClass == USB_CLASS_PRINTER) {
			ctx->found_printer = 1;
			ctx->current_iface_valid = 1;
			ctx->current_iface_num = iface->bInterfaceNumber;
			ctx->current_protocol = iface->bInterfaceProtocol;
			/*
			 * Record the interface number of the first printer
			 * interface whose bulk endpoints will be captured.
			 * A later printer interface in the walk must not
			 * override it, or the wIndex used for class-specific
			 * requests (GET_DEVICE_ID/GET_STATUS) would no longer
			 * match the endpoints collected above.
			 */
			if (!ctx->found_bulk_out && !ctx->found_bulk_in)
				ctx->iface_num = iface->bInterfaceNumber;
		} else {
			ctx->current_iface_valid = 0;
		}
		return 0;
	}

	if (!ctx->current_iface_valid)
		return 0;

	if (bDescriptorType == USB_DT_ENDPOINT && bLength >= 7) {
		const struct usb_endpoint_descriptor *ep =
			(const struct usb_endpoint_descriptor *)data;

		uint8_t ep_addr = ep->bEndpointAddress;
		uint8_t ep_attr = ep->bmAttributes;
		uint8_t ep_type = ep_attr & USB_ENDPOINT_XFERTYPE_MASK;

		if (ep_type != USB_ENDPOINT_XFER_BULK)
			return 0;

		if (ep_addr & USB_ENDPOINT_DIR_IN) {
			/* Bulk IN endpoint */
			if (!ctx->found_bulk_in) {
				ctx->bulk_in_ep = ep_addr;
				ctx->found_bulk_in = 1;
			}
		} else {
			/* Bulk OUT endpoint */
			if (!ctx->found_bulk_out) {
				ctx->bulk_out_ep = ep_addr;
				ctx->found_bulk_out = 1;
			}
		}
	}

	return 0;
}

/* ── Probe / Disconnect ──────────────────────────────────────────── */

static int usb_printer_probe(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return -EINVAL;

	/*
	 * Printer class is typically at the interface level.
	 * Match devices with class=0 (interface-defined class)
	 * or class=7 (device-level printer class).
	 */
	if (dev_desc->class_code != USB_CLASS_PRINTER &&
	    dev_desc->class_code != 0) {
		return -ENODEV;
	}

	if (g_printer_count >= MAX_PRINTERS) {
		kprintf("[printer] too many printer devices\n");
		return -ENOSPC;
	}

	/* Read configuration descriptor to find printer interface */
	uint8_t config_buf[PRINTER_CONFIG_BUF_SIZE];
	int ret;

	memset(config_buf, 0, sizeof(config_buf));

	/* Read config descriptor header (4 bytes for wTotalLength) */
	ret = usb_control_msg(dev_desc->addr,
			      USB_DIR_IN | USB_REQ_TYPE_STANDARD |
				  USB_REQ_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      (USB_DT_CONFIG << 8) | 0,
			      0, 4, config_buf);
	if (ret < 0) {
		kprintf("[printer] failed to read config header: %d\n", ret);
		return -ENODEV;
	}

	uint16_t total_len;
	memcpy(&total_len, config_buf + 2, sizeof(total_len));
	if (total_len > sizeof(config_buf)) {
		kprintf("[printer] config too large (%u), truncating\n",
			total_len);
		total_len = sizeof(config_buf);
	}

	/* Read full configuration descriptor */
	ret = usb_control_msg(dev_desc->addr,
			      USB_DIR_IN | USB_REQ_TYPE_STANDARD |
				  USB_REQ_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      (USB_DT_CONFIG << 8) | 0,
			      0, total_len, config_buf);
	if (ret < 0) {
		kprintf("[printer] failed to read full config: %d\n", ret);
		return -ENODEV;
	}

	/* Walk sub-descriptors to find printer interface and endpoints */
	struct printer_parse_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dev_addr = dev_desc->addr;
	ctx.vendor_id = dev_desc->vendor_id;
	ctx.product_id = dev_desc->product_id;
	ctx.bulk_out_ep = 0;
	ctx.bulk_in_ep = 0;

	ret = usb_for_each_config_subdesc(config_buf, total_len,
					  printer_desc_callback, &ctx);
	if (ret < 0 && ret != -ENOENT) {
		kprintf("[printer] descriptor walk error: %d\n", ret);
	}

	if (!ctx.found_printer || !ctx.found_bulk_out) {
		kprintf("[printer] no printer interface found for dev %u\n",
			dev_desc->addr);
		return -ENODEV;
	}

	/* Allocate printer device slot */
	struct usb_printer *p = &g_printers[g_printer_count];
	memset(p, 0, sizeof(*p));

	p->dev_addr = dev_desc->addr;
	p->vendor_id = dev_desc->vendor_id;
	p->product_id = dev_desc->product_id;
	p->iface_num = ctx.iface_num;
	p->bulk_out_ep = ctx.bulk_out_ep;
	p->bulk_in_ep = ctx.bulk_in_ep;
	p->protocol = ctx.current_protocol;
	p->bulk_out_toggle = PRINTER_BULK_TOGGLE_INIT;
	p->bulk_in_toggle = PRINTER_BULK_TOGGLE_INIT;
	p->present = 1;
	p->status = PRINTER_STATUS_NO_ERROR | PRINTER_STATUS_SELECTED;
	spinlock_init(&p->lock);

	/*
	 * Attempt to query the IEEE 1284 Device ID
	 * to identify the printer model.
	 */
	uint8_t dev_id_buf[256];
	memset(dev_id_buf, 0, sizeof(dev_id_buf));
	ret = request_device_id(p->dev_addr, p->iface_num,
				dev_id_buf, sizeof(dev_id_buf));
	if (ret > 0) {
		parse_device_id(p, dev_id_buf, ret);
	}

	/*
	 * Attempt to read the initial device status
	 * to populate the cached status byte.
	 */
	ret = request_status(p->dev_addr, p->iface_num);
	if (ret >= 0)
		p->status = (uint8_t)ret;

	/* Register /dev/usb/lpN */
	char devname[32];
	snprintf(devname, sizeof(devname), "usb/lp%d", g_printer_count);
	devfs_register_device(devname, (void *)(uintptr_t)(g_printer_count + 1),
			      NULL, NULL);

	int idx = g_printer_count;
	g_printer_count++;

	kprintf("[printer] USB printer registered: VID=0x%04x PID=0x%04x "
		"addr=%d iface=%d OUT=0x%02x IN=0x%02x proto=%d "
		"device_id=\"%s\"\n",
		dev_desc->vendor_id, dev_desc->product_id,
		p->dev_addr, p->iface_num,
		p->bulk_out_ep, p->bulk_in_ep, p->protocol,
		p->device_id_len > 0 ? p->device_id : "(none)");

	return 0;
}

static void usb_printer_disconnect(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return;

	/* Find and deactivate the printer device */
	for (int i = 0; i < g_printer_count; i++) {
		struct usb_printer *p = &g_printers[i];
		if (p->dev_addr == dev_desc->addr && p->present) {
			spinlock_acquire(&p->lock);

			p->present = 0;

			char devname[32];
			snprintf(devname, sizeof(devname), "usb/lp%d", i);
			devfs_unregister_device(devname);

			spinlock_release(&p->lock);

			kprintf("[printer] USB printer %d disconnected "
				"(VID=0x%04x PID=0x%04x)\n",
				i, p->vendor_id, p->product_id);
			break;
		}
	}
}

/* ── USB device ID table ─────────────────────────────────────────── */

static const struct usb_device_id usb_printer_ids[] = {
	/* Match USB Printer Class devices (class 0x07 at device level) */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			  USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			  USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_PRINTER,
	  .subclass = USB_PRINTER_SUBCLASS,
	  .protocol = USB_PRINTER_PROTO_UNIDIR },
	/* Bidirectional protocol */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			  USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			  USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_PRINTER,
	  .subclass = USB_PRINTER_SUBCLASS,
	  .protocol = USB_PRINTER_PROTO_BIDIR },
	/* IEEE 1284.4 protocol */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			  USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			  USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_PRINTER,
	  .subclass = USB_PRINTER_SUBCLASS,
	  .protocol = USB_PRINTER_PROTO_1284 },
	/* Also probe devices that use interface-level class */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
	  .class = 0x00, .subclass = 0x00, .protocol = 0x00 },
	USB_DEVICE_TABLE_END
};

/* ── Driver registration ─────────────────────────────────────────── */

static struct usb_driver g_usb_printer_driver = {
	.name       = "usb_printer",
	.id_table   = usb_printer_ids,
	.probe      = usb_printer_probe,
	.disconnect = usb_printer_disconnect,
};




/* ── Init / Exit ─────────────────────────────────────────────────── */

static void __init usb_printer_init(void)
{
	if (g_initialized)
		return;

	memset(g_printers, 0, sizeof(g_printers));
	g_printer_count = 0;
	g_initialized = 1;

	usb_register_driver(&g_usb_printer_driver);

	kprintf("[printer] USB Printer Class driver registered\n");
}

static void usb_printer_exit(void)
{
	usb_deregister_driver(&g_usb_printer_driver);

	/* Deactivate all devices */
	for (int i = 0; i < g_printer_count; i++) {
		struct usb_printer *p = &g_printers[i];
		spinlock_acquire(&p->lock);
		p->present = 0;
		spinlock_release(&p->lock);

		char devname[32];
		snprintf(devname, sizeof(devname), "usb/lp%d", i);
		devfs_unregister_device(devname);
	}

	g_printer_count = 0;
	g_initialized = 0;

	kprintf("[printer] USB Printer Class driver unregistered\n");
}

module_init(usb_printer_init);
module_exit(usb_printer_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.1.0");
MODULE_DESCRIPTION("USB Printer Class 1.2 driver with IEEE 1284 Device ID and "
		   "GET_STATUS support");
MODULE_AUTHOR("OS Kernel Team");
MODULE_ALIAS("usb:v* p* d* dc07 dsc01 dp*");       /* USB Printer class */
MODULE_ALIAS("usb:v* p* d* dc00 dsc* dp*");         /* Interface-level class */
