/*
 * kunit_usb.c — KUnit test suites for USB descriptor parsing.
 *
 * Validates the USB descriptor parsing API used during device
 * enumeration: device, config, interface, endpoint, and IAD
 * descriptors are parsed from raw bytes per the USB 2.0 spec.
 *
 * These tests run inside the running kernel and validate the
 * USB enumeration code path without requiring real hardware.
 */

#include "kunit.h"
#include "usb.h"
#include "string.h"
#include "printf.h"

/* ====================================================================
 *  1. Device descriptor parsing
 * ==================================================================== */

static void usb_parse_device_desc_test(struct kunit *test)
{
	/* Synthetic USB device descriptor (18 bytes, USB 2.0 spec §9.6.1) */
	uint8_t raw[18] = {
		18,             /* bLength */
		USB_DT_DEVICE,  /* bDescriptorType */
		0x00, 0x02,     /* bcdUSB = 0x0200 (USB 2.0) */
		0x00,           /* bDeviceClass (per-interface) */
		0x00,           /* bDeviceSubClass */
		0x00,           /* bDeviceProtocol */
		64,             /* bMaxPacketSize0 */
		0x09, 0x12,     /* idVendor = 0x1209 (Generic) */
		0x34, 0xAB,     /* idProduct = 0xAB34 */
		0x00, 0x01,     /* bcdDevice = 1.00 */
		0,              /* iManufacturer */
		0,              /* iProduct */
		0,              /* iSerialNumber */
		1               /* bNumConfigurations */
	};

	struct usb_device_descriptor desc;
	memset(&desc, 0, sizeof(desc));

	int ret = usb_parse_device_descriptor(raw, sizeof(raw), &desc);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.bLength, (int64_t)18);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.bDescriptorType, (int64_t)USB_DT_DEVICE);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.bcdUSB, (int64_t)0x0200);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.bMaxPacketSize0, (int64_t)64);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.idVendor, (int64_t)0x1209);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.idProduct, (int64_t)0xAB34);
	KUNIT_EXPECT_EQ(test, (int64_t)desc.bNumConfigurations, (int64_t)1);
}

static void usb_parse_device_desc_bad_length_test(struct kunit *test)
{
	uint8_t raw[18] = {
		5,              /* bLength too short (< 18) */
		USB_DT_DEVICE,
		0x00, 0x02,
	};

	struct usb_device_descriptor desc;
	memset(&desc, 0, sizeof(desc));

	int ret = usb_parse_device_descriptor(raw, sizeof(raw), &desc);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

static void usb_parse_device_desc_bad_type_test(struct kunit *test)
{
	uint8_t raw[18] = {
		18,
		0xFF,           /* not USB_DT_DEVICE */
		0x00, 0x02,
	};

	struct usb_device_descriptor desc;
	memset(&desc, 0, sizeof(desc));

	int ret = usb_parse_device_descriptor(raw, sizeof(raw), &desc);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

static void usb_parse_device_desc_null_test(struct kunit *test)
{
	struct usb_device_descriptor desc;
	memset(&desc, 0, sizeof(desc));

	int ret = usb_parse_device_descriptor(NULL, 0, &desc);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	ret = usb_parse_device_descriptor((const uint8_t *)"12345678", 8, NULL);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

/* ====================================================================
 *  2. Configuration descriptor parsing
 * ==================================================================== */

static void usb_parse_config_desc_test(struct kunit *test)
{
	uint8_t raw[] = {
		9,              /* bLength */
		USB_DT_CONFIG,  /* bDescriptorType */
		32, 0,          /* wTotalLength = 32 */
		1,              /* bNumInterfaces */
		1,              /* bConfigurationValue */
		0,              /* iConfiguration */
		0x80,           /* bmAttributes (bus-powered, no remote wakeup) */
		50              /* bMaxPower = 100 mA */
	};

	struct usb_config_descriptor config;
	memset(&config, 0, sizeof(config));

	int ret = usb_parse_config_descriptor(raw, sizeof(raw), &config);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)config.bLength, (int64_t)9);
	KUNIT_EXPECT_EQ(test, (int64_t)config.bDescriptorType, (int64_t)USB_DT_CONFIG);
	KUNIT_EXPECT_EQ(test, (int64_t)config.wTotalLength, (int64_t)32);
	KUNIT_EXPECT_EQ(test, (int64_t)config.bNumInterfaces, (int64_t)1);
	KUNIT_EXPECT_EQ(test, (int64_t)config.bConfigurationValue, (int64_t)1);
	KUNIT_EXPECT_EQ(test, (int64_t)config.bMaxPower, (int64_t)50);
}

static void usb_parse_config_desc_bad_test(struct kunit *test)
{
	/* Too short */
	uint8_t raw[] = { 4, USB_DT_CONFIG, 0, 0 };
	struct usb_config_descriptor config;
	memset(&config, 0, sizeof(config));

	int ret = usb_parse_config_descriptor(raw, sizeof(raw), &config);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	/* Wrong type */
	uint8_t raw2[] = { 9, USB_DT_DEVICE, 32, 0, 1, 1, 0, 0x80, 50 };
	ret = usb_parse_config_descriptor(raw2, sizeof(raw2), &config);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	/* NULL */
	ret = usb_parse_config_descriptor(NULL, 9, &config);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

/* ====================================================================
 *  3. Interface descriptor parsing
 * ==================================================================== */

static void usb_parse_interface_desc_test(struct kunit *test)
{
	uint8_t raw[] = {
		9,                  /* bLength */
		USB_DT_INTERFACE,   /* bDescriptorType */
		0,                  /* bInterfaceNumber */
		0,                  /* bAlternateSetting */
		2,                  /* bNumEndpoints */
		USB_CLASS_HID,      /* bInterfaceClass (HID) */
		0,                  /* bInterfaceSubClass */
		1,                  /* bInterfaceProtocol (keyboard) */
		0                   /* iInterface */
	};

	struct usb_interface_descriptor iface;
	memset(&iface, 0, sizeof(iface));

	int ret = usb_parse_interface_descriptor(raw, &iface);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)iface.bLength, (int64_t)9);
	KUNIT_EXPECT_EQ(test, (int64_t)iface.bDescriptorType, (int64_t)USB_DT_INTERFACE);
	KUNIT_EXPECT_EQ(test, (int64_t)iface.bInterfaceNumber, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)iface.bNumEndpoints, (int64_t)2);
	KUNIT_EXPECT_EQ(test, (int64_t)iface.bInterfaceClass, (int64_t)USB_CLASS_HID);
}

/* ====================================================================
 *  4. Endpoint descriptor parsing
 * ==================================================================== */

static void usb_parse_endpoint_desc_test(struct kunit *test)
{
	uint8_t raw[] = {
		7,                  /* bLength */
		USB_DT_ENDPOINT,    /* bDescriptorType */
		0x81,               /* bEndpointAddress: EP1 IN */
		USB_ENDPOINT_XFER_INT, /* bmAttributes: Interrupt */
		64, 0,              /* wMaxPacketSize = 64 */
		10                  /* bInterval = 10 ms */
	};

	struct usb_endpoint_descriptor ep;
	memset(&ep, 0, sizeof(ep));

	int ret = usb_parse_endpoint_descriptor(raw, &ep);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.bLength, (int64_t)7);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.bDescriptorType, (int64_t)USB_DT_ENDPOINT);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.bEndpointAddress, (int64_t)0x81);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.bmAttributes, (int64_t)USB_ENDPOINT_XFER_INT);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.wMaxPacketSize, (int64_t)64);
	KUNIT_EXPECT_EQ(test, (int64_t)ep.bInterval, (int64_t)10);
}

static void usb_parse_endpoint_desc_bad_test(struct kunit *test)
{
	/* Too short */
	uint8_t raw[] = { 4, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_XFER_INT };
	struct usb_endpoint_descriptor ep;
	memset(&ep, 0, sizeof(ep));

	int ret = usb_parse_endpoint_descriptor(raw, &ep);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	/* Wrong type */
	uint8_t raw2[] = { 7, USB_DT_CONFIG, 0x81, USB_ENDPOINT_XFER_INT, 64, 0, 10 };
	ret = usb_parse_endpoint_descriptor(raw2, &ep);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

/* ====================================================================
 *  5. Interface Association Descriptor (IAD) parsing
 * ==================================================================== */

static void usb_parse_iad_desc_test(struct kunit *test)
{
	uint8_t raw[] = {
		8,                      /* bLength */
		USB_DT_INTERFACE_ASSOC, /* bDescriptorType */
		0,                      /* bFirstInterface */
		2,                      /* bInterfaceCount */
		USB_CLASS_AUDIO,        /* bFunctionClass */
		USB_CLASS_AUDIO_STREAM, /* bFunctionSubClass */
		0,                      /* bFunctionProtocol */
		0                       /* iFunction */
	};

	struct usb_iad_descriptor iad;
	memset(&iad, 0, sizeof(iad));

	int ret = usb_parse_iad_descriptor(raw, &iad);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)iad.bLength, (int64_t)8);
	KUNIT_EXPECT_EQ(test, (int64_t)iad.bDescriptorType, (int64_t)USB_DT_INTERFACE_ASSOC);
	KUNIT_EXPECT_EQ(test, (int64_t)iad.bFirstInterface, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)iad.bInterfaceCount, (int64_t)2);
	KUNIT_EXPECT_EQ(test, (int64_t)iad.bFunctionClass, (int64_t)USB_CLASS_AUDIO);
}

/* ====================================================================
 *  6. Configuration sub-descriptor iteration
 * ==================================================================== */

/* Callback that counts how many times it's called and checks descriptor types */
struct count_cb_data {
	int count;
	int last_type;
};

static int count_callback(uint8_t bDescriptorType, const uint8_t *data,
                           uint8_t bLength, void *user_data)
{
	struct count_cb_data *cb = (struct count_cb_data *)user_data;
	(void)data;
	cb->count++;
	cb->last_type = (int)bDescriptorType;
	(void)bLength;
	return 0;
}

static void usb_for_each_config_subdesc_test(struct kunit *test)
{
	/*
	 * Synthetic config descriptor with:
	 * - 9-byte config header
	 * - 9-byte interface descriptor
	 * - 7-byte endpoint descriptor
	 * Total = 25 bytes
	 */
	uint8_t raw[32];
	uint16_t off = 0;

	/* Config descriptor header */
	raw[off++] = 9;
	raw[off++] = USB_DT_CONFIG;
	raw[off++] = 25; /* wTotalLength low */
	raw[off++] = 0;  /* wTotalLength high */
	raw[off++] = 1;  /* bNumInterfaces */
	raw[off++] = 1;  /* bConfigurationValue */
	raw[off++] = 0;  /* iConfiguration */
	raw[off++] = 0x80;
	raw[off++] = 50; /* bMaxPower */

	/* Interface descriptor */
	raw[off++] = 9;
	raw[off++] = USB_DT_INTERFACE;
	raw[off++] = 0;  /* bInterfaceNumber */
	raw[off++] = 0;  /* bAlternateSetting */
	raw[off++] = 1;  /* bNumEndpoints */
	raw[off++] = 0xFF; /* bInterfaceClass (vendor-specific) */
	raw[off++] = 0;
	raw[off++] = 0;
	raw[off++] = 0;

	/* Endpoint descriptor */
	raw[off++] = 7;
	raw[off++] = USB_DT_ENDPOINT;
	raw[off++] = 0x81; /* EP1 IN */
	raw[off++] = USB_ENDPOINT_XFER_BULK;
	raw[off++] = 64;
	raw[off++] = 0;
	raw[off++] = 0;

	struct count_cb_data cb_data;
	memset(&cb_data, 0, sizeof(cb_data));

	int ret = usb_for_each_config_subdesc(raw, off, count_callback, &cb_data);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)cb_data.count, (int64_t)2);  /* interface + endpoint */
	KUNIT_EXPECT_EQ(test, (int64_t)cb_data.last_type, (int64_t)USB_DT_ENDPOINT);
}

static void usb_for_each_config_subdesc_bad_test(struct kunit *test)
{
	/* NULL params */
	int ret = usb_for_each_config_subdesc(NULL, 10, count_callback, NULL);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	/* Too short total_length */
	uint8_t raw[] = { 9, USB_DT_CONFIG, 9, 0, 1, 1, 0, 0x80, 50 };
	ret = usb_for_each_config_subdesc(raw, 2, count_callback, NULL);
	KUNIT_EXPECT_TRUE(test, ret < 0);

	/* Wrong descriptor type at start */
	uint8_t raw2[] = { 9, USB_DT_DEVICE, 9, 0, 1, 1, 0, 0x80, 50 };
	ret = usb_for_each_config_subdesc(raw2, sizeof(raw2), count_callback, NULL);
	KUNIT_EXPECT_TRUE(test, ret < 0);
}

/* ====================================================================
 *  7. Device update from descriptor
 * ==================================================================== */

static void usb_update_device_test(struct kunit *test)
{
	/* Create a device descriptor */
	struct usb_device_descriptor dev_desc;
	memset(&dev_desc, 0, sizeof(dev_desc));
	dev_desc.bLength = 18;
	dev_desc.bDescriptorType = USB_DT_DEVICE;
	dev_desc.bcdUSB = 0x0200;
	dev_desc.bDeviceClass = USB_CLASS_HID;
	dev_desc.bDeviceSubClass = 0;
	dev_desc.bDeviceProtocol = 0;
	dev_desc.idVendor = 0x1209;
	dev_desc.idProduct = 0xABCD;

	/* Create a device struct */
	struct usb_device dev;
	memset(&dev, 0, sizeof(dev));
	dev.addr = 5;
	dev.speed = 2;  /* high speed */

	usb_update_device_from_desc(&dev, &dev_desc);

	/* Verify fields were updated */
	KUNIT_EXPECT_EQ(test, (int64_t)dev.vendor_id, (int64_t)0x1209);
	KUNIT_EXPECT_EQ(test, (int64_t)dev.product_id, (int64_t)0xABCD);
	KUNIT_EXPECT_EQ(test, (int64_t)dev.class_code, (int64_t)USB_CLASS_HID);
	KUNIT_EXPECT_NE(test, (int64_t)(dev.flags & USB_DEV_FLAG_HAS_DESC), (int64_t)0);

	/* Verify addr/speed weren't clobbered */
	KUNIT_EXPECT_EQ(test, (int64_t)dev.addr, (int64_t)5);
	KUNIT_EXPECT_EQ(test, (int64_t)dev.speed, (int64_t)2);

	/* NULL safety */
	usb_update_device_from_desc(NULL, &dev_desc);
	usb_update_device_from_desc(&dev, NULL);
}

/* ====================================================================
 *  Test case lists
 * ==================================================================== */

static struct kunit_case usb_parse_device_cases[] = {
	KUNIT_CASE(usb_parse_device_desc_test),
	KUNIT_CASE(usb_parse_device_desc_bad_length_test),
	KUNIT_CASE(usb_parse_device_desc_bad_type_test),
	KUNIT_CASE(usb_parse_device_desc_null_test),
	{0}
};

static struct kunit_case usb_parse_config_cases[] = {
	KUNIT_CASE(usb_parse_config_desc_test),
	KUNIT_CASE(usb_parse_config_desc_bad_test),
	{0}
};

static struct kunit_case usb_parse_interface_cases[] = {
	KUNIT_CASE(usb_parse_interface_desc_test),
	{0}
};

static struct kunit_case usb_parse_endpoint_cases[] = {
	KUNIT_CASE(usb_parse_endpoint_desc_test),
	KUNIT_CASE(usb_parse_endpoint_desc_bad_test),
	{0}
};

static struct kunit_case usb_parse_iad_cases[] = {
	KUNIT_CASE(usb_parse_iad_desc_test),
	{0}
};

static struct kunit_case usb_iter_config_cases[] = {
	KUNIT_CASE(usb_for_each_config_subdesc_test),
	KUNIT_CASE(usb_for_each_config_subdesc_bad_test),
	{0}
};

static struct kunit_case usb_update_device_cases[] = {
	KUNIT_CASE(usb_update_device_test),
	{0}
};

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

static struct kunit_suite usb_parse_device_suite;
static struct kunit_suite usb_parse_config_suite;
static struct kunit_suite usb_parse_interface_suite;
static struct kunit_suite usb_parse_endpoint_suite;
static struct kunit_suite usb_parse_iad_suite;
static struct kunit_suite usb_iter_config_suite;
static struct kunit_suite usb_update_device_suite;

/* ====================================================================
 *  Registration
 * ==================================================================== */

void kunit_usb_register(void)
{
	/* ── Device descriptor parsing ── */
	{
		int ci = 0;
		while (usb_parse_device_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_parse_device_suite.cases[ci].name = usb_parse_device_cases[ci].name;
			usb_parse_device_suite.cases[ci].run  = usb_parse_device_cases[ci].run;
			ci++;
		}
		usb_parse_device_suite.cases[ci].name = NULL;
		usb_parse_device_suite.cases[ci].run  = NULL;
		usb_parse_device_suite.name     = "usb_parse_device";
		usb_parse_device_suite.setup    = NULL;
		usb_parse_device_suite.teardown = NULL;
		kunit_register_suite(&usb_parse_device_suite);
	}

	/* ── Configuration descriptor parsing ── */
	{
		int ci = 0;
		while (usb_parse_config_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_parse_config_suite.cases[ci].name = usb_parse_config_cases[ci].name;
			usb_parse_config_suite.cases[ci].run  = usb_parse_config_cases[ci].run;
			ci++;
		}
		usb_parse_config_suite.cases[ci].name = NULL;
		usb_parse_config_suite.cases[ci].run  = NULL;
		usb_parse_config_suite.name     = "usb_parse_config";
		usb_parse_config_suite.setup    = NULL;
		usb_parse_config_suite.teardown = NULL;
		kunit_register_suite(&usb_parse_config_suite);
	}

	/* ── Interface descriptor parsing ── */
	{
		int ci = 0;
		while (usb_parse_interface_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_parse_interface_suite.cases[ci].name = usb_parse_interface_cases[ci].name;
			usb_parse_interface_suite.cases[ci].run  = usb_parse_interface_cases[ci].run;
			ci++;
		}
		usb_parse_interface_suite.cases[ci].name = NULL;
		usb_parse_interface_suite.cases[ci].run  = NULL;
		usb_parse_interface_suite.name     = "usb_parse_interface";
		usb_parse_interface_suite.setup    = NULL;
		usb_parse_interface_suite.teardown = NULL;
		kunit_register_suite(&usb_parse_interface_suite);
	}

	/* ── Endpoint descriptor parsing ── */
	{
		int ci = 0;
		while (usb_parse_endpoint_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_parse_endpoint_cases[ci].name = usb_parse_endpoint_cases[ci].name;
			usb_parse_endpoint_cases[ci].run  = usb_parse_endpoint_cases[ci].run;
			ci++;
		}
		usb_parse_endpoint_suite.cases[ci].name = NULL;
		usb_parse_endpoint_suite.cases[ci].run  = NULL;
		usb_parse_endpoint_suite.name     = "usb_parse_endpoint";
		usb_parse_endpoint_suite.setup    = NULL;
		usb_parse_endpoint_suite.teardown = NULL;
		kunit_register_suite(&usb_parse_endpoint_suite);
	}

	/* ── IAD descriptor parsing ── */
	{
		int ci = 0;
		while (usb_parse_iad_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_parse_iad_cases[ci].name = usb_parse_iad_cases[ci].name;
			usb_parse_iad_cases[ci].run  = usb_parse_iad_cases[ci].run;
			ci++;
		}
		usb_parse_iad_suite.cases[ci].name = NULL;
		usb_parse_iad_suite.cases[ci].run  = NULL;
		usb_parse_iad_suite.name     = "usb_parse_iad";
		usb_parse_iad_suite.setup    = NULL;
		usb_parse_iad_suite.teardown = NULL;
		kunit_register_suite(&usb_parse_iad_suite);
	}

	/* ── Config sub-descriptor iteration ── */
	{
		int ci = 0;
		while (usb_iter_config_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_iter_config_suite.cases[ci].name = usb_iter_config_cases[ci].name;
			usb_iter_config_suite.cases[ci].run  = usb_iter_config_cases[ci].run;
			ci++;
		}
		usb_iter_config_suite.cases[ci].name = NULL;
		usb_iter_config_suite.cases[ci].run  = NULL;
		usb_iter_config_suite.name     = "usb_iter_config";
		usb_iter_config_suite.setup    = NULL;
		usb_iter_config_suite.teardown = NULL;
		kunit_register_suite(&usb_iter_config_suite);
	}

	/* ── Device update from descriptor ── */
	{
		int ci = 0;
		while (usb_update_device_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			usb_update_device_suite.cases[ci].name = usb_update_device_cases[ci].name;
			usb_update_device_suite.cases[ci].run  = usb_update_device_cases[ci].run;
			ci++;
		}
		usb_update_device_suite.cases[ci].name = NULL;
		usb_update_device_suite.cases[ci].run  = NULL;
		usb_update_device_suite.name     = "usb_update_device";
		usb_update_device_suite.setup    = NULL;
		usb_update_device_suite.teardown = NULL;
		kunit_register_suite(&usb_update_device_suite);
	}
}
