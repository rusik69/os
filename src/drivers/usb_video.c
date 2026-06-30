/*
 * usb_video.c — USB Video Class 1.5 (UVC) driver
 *
 * Implements USB Video Class (UVC) 1.5 specification for video streaming
 * devices (webcams, video capture devices). Supports VideoControl interface
 * descriptor parsing (camera terminals, processing units, extension units),
 * VideoStreaming format/colorimetry/frame descriptor parsing, class-specific
 * requests (probe/commit, controls), and isochronous video frame capture.
 *
 * UVC 1.5 specification references:
 *   USB Device Class Definition for Video Devices, Rev 1.5
 *   USB Video Payload Formats, Rev 1.5
 *   USB Video Class 1.5 Extended Colorimetry
 *   USB Video Class 1.5 Frame-Based Payload
 *   MJPEG, uncompressed (YUY2), and H.264 payload formats
 */

#define KERNEL_INTERNAL
#include "usb.h"
#include "usb_core.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pmm.h"
#include "heap.h"
#include "module.h"
#include "types.h"

/* ── UVC class constants ──────────────────────────────────────────────── */

#define UVC_VERSION_1_0            0x0100
#define UVC_VERSION_1_1            0x0110
#define UVC_VERSION_1_5            0x0150

/* USB Video Class (UVC) uses class code 0x0E */
#define USB_CLASS_VIDEO            0x0E

/* Video interface subclass codes */
#define UVC_SUBCLASS_UNDEFINED     0x00
#define UVC_SUBCLASS_CONTROL       0x01   /* VideoControl interface */
#define UVC_SUBCLASS_STREAMING     0x02   /* VideoStreaming interface */
#define UVC_SUBCLASS_INTERRUPT     0x03   /* VideoCollection interface */

/* Video interface protocol codes */
#define UVC_PROTOCOL_1_0           0x00
#define UVC_PROTOCOL_1_5           0x01   /* UVC 1.5 uses protocol = 0x01 */

/* ── VideoControl descriptor subtypes (CS_INTERFACE, 0x24) ────────────── */

#define CS_INTERFACE              0x24
#define CS_ENDPOINT               0x25

/* VideoControl interface descriptor subtypes */
#define VC_HEADER                 0x01
#define VC_INPUT_TERMINAL         0x02
#define VC_OUTPUT_TERMINAL        0x03
#define VC_SELECTOR_UNIT          0x04
#define VC_PROCESSING_UNIT        0x05
#define VC_EXTENSION_UNIT         0x06
#define VC_ENCODER_UNIT           0x07
#define VC_DECODER_UNIT           0x08

/* ── VideoStreaming descriptor subtypes (CS_INTERFACE) ────────────────── */

#define VS_INPUT_HEADER           0x01
#define VS_OUTPUT_HEADER          0x02
#define VS_STILL_IMAGE_FRAME      0x03
#define VS_FORMAT_UNCOMPRESSED    0x04
#define VS_FORMAT_MJPEG           0x05
#define VS_FRAME_UNCOMPRESSED     0x06
#define VS_FRAME_MJPEG            0x07
#define VS_FORMAT_MPEG2TS         0x0A
#define VS_FORMAT_DV              0x0C
#define VS_FORMAT_FRAME_BASED     0x10  /* UVC 1.5: H.264, etc. */
#define VS_FRAME_FRAME_BASED      0x11
#define VS_FORMAT_STREAM_BASED    0x12
#define VS_FORMAT_H264            0x13  /* UVC 1.5 H.264 */
#define VS_FRAME_H264             0x14
#define VS_FORMAT_H264_SIMULCAST  0x15
#define VS_FORMAT_VP8             0x16  /* UVC 1.5 VP8 */
#define VS_FRAME_VP8              0x17
#define VS_FORMAT_VP8_SIMULCAST   0x18
#define VS_COLOR_FORMAT           0x20  /* UVC 1.5 colorimetry descriptor */

/* ── Video class-specific request codes ────────────────────────────────── */

/* Standard UVC requests (VideoControl interface) */
#define UVC_REQ_SET_CUR           0x01
#define UVC_REQ_GET_CUR           0x81
#define UVC_REQ_SET_MIN           0x02
#define UVC_REQ_GET_MIN           0x82
#define UVC_REQ_SET_MAX           0x03
#define UVC_REQ_GET_MAX           0x83
#define UVC_REQ_SET_RES           0x04
#define UVC_REQ_GET_RES           0x84
#define UVC_REQ_SET_MEM           0x05
#define UVC_REQ_GET_MEM           0x85
#define UVC_REQ_GET_INFO          0x86
#define UVC_REQ_GET_DEF           0x87

/* Request type values for video class requests */
#define UVC_REQTYPE_SET_CUR       (USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)
#define UVC_REQTYPE_GET_CUR       (USB_DIR_IN  | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)

/* ── VideoControl processing unit control selectors ────────────────────── */

#define PU_CONTROL_UNDEFINED       0x00
#define PU_BACKLIGHT_COMPENSATION  0x01
#define PU_BRIGHTNESS              0x02
#define PU_CONTRAST                0x03
#define PU_GAIN                    0x04
#define PU_POWER_LINE_FREQUENCY    0x05
#define PU_HUE                     0x06
#define PU_SATURATION              0x07
#define PU_SHARPNESS               0x08
#define PU_GAMMA                   0x09
#define PU_WHITE_BALANCE_TEMPERATURE 0x0A
#define PU_WHITE_BALANCE_COMPONENT 0x0B
#define PU_BACKLIGHT_COMPENSATION_2 0x0C
#define PU_GAIN_AUTOMATIC          0x0D
#define PU_HUE_AUTOMATIC           0x0E
#define PU_WHITE_BALANCE_AUTOMATIC 0x0F
#define PU_FOCUS                   0x10
#define PU_FOCUS_AUTOMATIC         0x11
#define PU_IRIS                    0x12
#define PU_IRIS_AUTOMATIC          0x13
#define PU_ZOOM                    0x14
#define PU_PAN                     0x15
#define PU_TILT                    0x16
#define PU_ROLL                    0x17
#define PU_PRIVACY                 0x18
#define PU_DIGITAL_WINDOW          0x19
#define PU_REGION_OF_INTEREST      0x1A

/* ── Camera terminal control selectors ────────────────────────────────── */

#define CT_CONTROL_UNDEFINED       0x00
#define CT_SCANNING_MODE           0x01
#define CT_AE_MODE                 0x02
#define CT_AE_PRIORITY             0x03
#define CT_EXPOSURE_TIME           0x04
#define CT_FOCUS_ABSOLUTE          0x05
#define CT_FOCUS_RELATIVE          0x06
#define CT_IRIS_ABSOLUTE           0x07
#define CT_IRIS_RELATIVE           0x08
#define CT_ZOOM_ABSOLUTE           0x09
#define CT_ZOOM_RELATIVE           0x0A
#define CT_PANTILT_ABSOLUTE        0x0B
#define CT_PANTILT_RELATIVE        0x0C
#define CT_ROLL_ABSOLUTE           0x0D
#define CT_ROLL_RELATIVE           0x0E
#define CT_PRIVACY                 0x0F
#define CT_FOCUS_AUTO              0x10

/* ── Video terminal types (wTerminalType) ─────────────────────────────── */

#define VC_TERMINAL_UNDEFINED        0x0100
#define VC_TERMINAL_STREAMING        0x0101
#define VC_TERMINAL_VENDOR           0x01FF

#define VC_INPUT_UNDEFINED           0x0200
#define VC_INPUT_CAMERA              0x0201
#define VC_INPUT_MEDIA_SENSOR        0x0202

#define VC_OUTPUT_UNDEFINED          0x0300
#define VC_OUTPUT_DISPLAY            0x0301
#define VC_OUTPUT_SVIDEO             0x0302
#define VC_OUTPUT_COMPOSITE          0x0303
#define VC_OUTPUT_COMPONENT          0x0304

#define VC_EXTERNAL_UNDEFINED        0x0400
#define VC_EXTERNAL_COMPOSITE        0x0401
#define VC_EXTERNAL_SVIDEO           0x0402
#define VC_EXTERNAL_COMPONENT        0x0403

/* ── limits ───────────────────────────────────────────────────────────── */

#define UVC_MAX_TERMINALS            8   /* max terminals per video function */
#define UVC_MAX_UNITS                8   /* max processing/extension units */
#define UVC_MAX_FORMATS              8   /* max formats per streaming iface */
#define UVC_MAX_FRAMES              16   /* max frame descriptors per format */
#define UVC_MAX_RESOLUTIONS         16   /* max resolution entries */
#define UVC_MAX_DEVICES              4   /* max video devices tracked */
#define UVC_MAX_FRAME_SIZE          (1920 * 1080 * 3) /* max raw frame buffer */
#define UVC_MAX_CONTROLS            32   /* max cached control values */
#define UVC_VS_PROBE_SIZE           26   /* VS_PROBE_CONTROL (UVC 1.5) */
#define UVC_VS_COMMIT_SIZE          26   /* VS_COMMIT_CONTROL (UVC 1.5) */
#define UVC_STILL_PROBE_SIZE        26   /* VS_STILL_PROBE_CONTROL */

/* ── Terminal / unit types for disambiguation ─────────────────────────── */

#define UVC_ENTITY_TERMINAL         0
#define UVC_ENTITY_PROCESSING_UNIT  1
#define UVC_ENTITY_EXTENSION_UNIT   2
#define UVC_ENTITY_SELECTOR_UNIT    3
#define UVC_ENTITY_ENCODER_UNIT     4
#define UVC_ENTITY_DECODER_UNIT     5

/* ── Video streaming interface control selectors ──────────────────────── */

#define VS_CONTROL_UNDEFINED        0x00
#define VS_PROBE_CONTROL            0x01
#define VS_COMMIT_CONTROL           0x02
#define VS_STILL_PROBE_CONTROL      0x03
#define VS_STILL_COMMIT_CONTROL     0x04
#define VS_STILL_IMAGE_TRIGGER      0x05
#define VS_STREAM_ERROR_CODE        0x06
#define VS_GENERATE_KEY_FRAME       0x07
#define VS_UPDATE_FRAME_SEGMENT     0x08
#define VS_SYNCH_DELAY_CONTROL      0x09

/* ── UVC 1.5 extended colorimetry constants ───────────────────────────── */

#define UVC_COLOR_PRIM_UNDEFINED     0x00
#define UVC_COLOR_PRIM_BT709         0x01
#define UVC_COLOR_PRIM_BT470_2M      0x02
#define UVC_COLOR_PRIM_BT470_2G      0x03
#define UVC_COLOR_PRIM_SMPTE170M     0x04
#define UVC_COLOR_PRIM_SMPTE240M     0x05
#define UVC_COLOR_PRIM_SRGB          0x06  /* UVC 1.5 */

#define UVC_TRANSFER_CHAR_UNDEFINED  0x00
#define UVC_TRANSFER_CHAR_BT709      0x01
#define UVC_TRANSFER_CHAR_BT470_2M   0x02
#define UVC_TRANSFER_CHAR_BT470_2G   0x03
#define UVC_TRANSFER_CHAR_SMPTE170M  0x04
#define UVC_TRANSFER_CHAR_SMPTE240M  0x05
#define UVC_TRANSFER_CHAR_SRGB       0x06  /* UVC 1.5 */

#define UVC_MATRIX_COEFF_UNDEFINED   0x00
#define UVC_MATRIX_COEFF_BT709       0x01
#define UVC_MATRIX_COEFF_FCC         0x02
#define UVC_MATRIX_COEFF_BT470_2G    0x03
#define UVC_MATRIX_COEFF_SMPTE170M   0x04
#define UVC_MATRIX_COEFF_SMPTE240M   0x05

/* ── Color matching descriptor structure (UVC 1.5) ────────────────────── */

struct uvc_color_matching {
	uint8_t  bLength;
	uint8_t  bDescriptorType;    /* CS_INTERFACE */
	uint8_t  bDescriptorSubType; /* VS_COLOR_FORMAT (0x20) */
	uint8_t  bColorPrimaries;
	uint8_t  bTransferCharacteristics;
	uint8_t  bMatrixCoefficients;
} __attribute__((packed));

/* ── UVC extension unit descriptor (variable length) ──────────────────── */
/* Parsed from descriptor data dynamically — not __attribute__((packed)). */

/* ── Video terminal record (input or output) ──────────────────────────── */

struct uvc_terminal {
	uint8_t  id;              /* terminal ID */
	uint16_t terminal_type;   /* wTerminalType */
	uint8_t  is_input;        /* 1 = input terminal, 0 = output terminal */
	uint8_t  assoc_terminal;  /* bAssocTerminal */
	/* Camera-specific fields (for VC_INPUT_CAMERA) */
	uint16_t focal_min;       /* wObjectiveFocalLengthMin */
	uint16_t focal_max;       /* wObjectiveFocalLengthMax */
	uint16_t ocal_focal;      /* wOcularFocalLength */
	uint8_t  control_size;    /* bControlSize */
	uint32_t controls;        /* bmControls bitmap (up to 4 bytes) */
};

/* ── Processing unit record ───────────────────────────────────────────── */

struct uvc_processing_unit {
	uint8_t  id;              /* bUnitID */
	uint8_t  source_id;       /* bSourceID */
	uint16_t max_multiplier;  /* dwMaxMultiplier (×10000) */
	uint8_t  control_size;    /* bControlSize */
	uint32_t controls;        /* bmControls bitmap */
	uint8_t  unit_index;      /* iProcessing string descriptor */
};

/* ── Extension unit record ────────────────────────────────────────────── */

struct uvc_extension_unit {
	uint8_t  id;              /* bUnitID */
	uint8_t  source_id;       /* bSourceID */
	uint16_t guid[8];         /* GUID identifying the extension */
	uint8_t  num_controls;    /* bNumControls */
	uint8_t  control_size;    /* bControlSize */
	uint32_t controls;        /* bmControls bitmap */
	uint8_t  unit_index;      /* iExtension string descriptor */
};

/* ── Frame descriptor (per resolution) ────────────────────────────────── */

struct uvc_frame_desc {
	uint16_t width;           /* wWidth */
	uint16_t height;          /* wHeight */
	uint32_t min_bit_rate;    /* dwMinBitRate */
	uint32_t max_bit_rate;    /* dwMaxBitRate */
	uint32_t max_frame_size;  /* dwMaxVideoFrameBufferSize */
	uint32_t default_frame_interval;  /* dwDefaultFrameInterval */
	uint32_t min_interval;    /* dwMinFrameInterval (100 ns units) */
	uint32_t max_interval;    /* dwMaxFrameInterval */
	uint32_t interval_step;   /* dwFrameIntervalStep */
	uint8_t  intervals_discrete; /* bFrameIntervalType (0 = continuous) */
};

/* ── Format descriptor ────────────────────────────────────────────────── */

struct uvc_format_desc {
	uint8_t  format_index;    /* bFormatIndex */
	uint8_t  num_frame_descs; /* bNumFrameDescriptors */
	uint8_t  guid[16];        /* GUID specifying the format */
	uint8_t  bits_per_pixel;  /* bBitsPerPixel */
	uint8_t  default_frame_index; /* bDefaultFrameIndex */
	uint8_t  aspect_ratio_x;  /* bAspectRatioX */
	uint8_t  aspect_ratio_y;  /* bAspectRatioY */
	uint8_t  interlace_flags; /* bmInterlaceFlags */
	uint8_t  copy_protect;    /* bCopyProtect */
	uint8_t  variable_size;   /* bVariableSize (UVC 1.5 frame-based) */
	/* UVC 1.5 extended fields */
	uint8_t  color_primaries; /* bColorPrimaries */
	uint8_t  transfer_chars;  /* bTransferCharacteristics */
	uint8_t  matrix_coeff;    /* bMatrixCoefficients */
	/* Frame descriptors */
	int      num_frames;
	struct uvc_frame_desc frames[UVC_MAX_FRAMES];
	/* Color matching */
	struct uvc_color_matching color_matching;
};

/* ── Video device instance ────────────────────────────────────────────── */

struct uvc_device {
	uint8_t  dev_addr;        /* USB device address */
	uint8_t  vc_iface;        /* VideoControl interface number */
	uint8_t  vs_iface;        /* VideoStreaming interface number (first) */
	uint8_t  alt_setting;     /* alternate setting for streaming */
	uint16_t uvc_version;     /* UVC_VERSION_1_0, _1_5, etc. */
	int      active;          /* 1 if device is initialised */

	/* Topology */
	int      num_terminals;
	struct uvc_terminal terminals[UVC_MAX_TERMINALS];

	int      num_processing_units;
	struct uvc_processing_unit processing_units[UVC_MAX_UNITS];

	int      num_extension_units;
	struct uvc_extension_unit extension_units[UVC_MAX_UNITS];

	/* Streaming info */
	int      num_formats;
	struct uvc_format_desc formats[UVC_MAX_FORMATS];

	uint8_t  iso_in_ep;       /* isochronous IN endpoint */
	uint8_t  iso_out_ep;      /* isochronous OUT endpoint */
	uint16_t max_packet_size;
	uint8_t  bStillCaptureMethod; /* bStillCaptureMethod */
	uint8_t  bTriggerSupport;
	uint8_t  bTriggerUsage;

	/* Probe / Commit state */
	uint8_t  probe_buf[UVC_VS_PROBE_SIZE];
	uint8_t  commit_buf[UVC_VS_COMMIT_SIZE];
	int      streaming_active;

	/* Frame buffering */
	uint8_t *frame_buffer;
	uint32_t frame_buffer_size;
	uint32_t frame_bytes_received;
	uint32_t max_frame_size;

	/* Cached control values */
	struct uvc_cached_control {
		uint8_t  entity_id;
		uint8_t  selector;
		int32_t  value;
		int      valid;
	} controls[UVC_MAX_CONTROLS];
	int num_cached_controls;
};

/* ── UVC request header structure (bmRequestType format) ──────────────── */

struct uvc_request_header {
	uint8_t  bmRequestType;
	uint8_t  bRequest;
	uint16_t wValue;      /* (control_selector << 8) | entity_id */
	uint16_t wIndex;      /* interface number or endpoint */
	uint16_t wLength;     /* data payload length */
};

/* ── Video probe/commit control structure (VS_PROBE_CONTROL / VS_COMMIT_CONTROL) */
/* UVC 1.5 uses 26-byte probe/commit (was 34 in earlier drafts, 26 in final spec) */

struct uvc_probe_commit {
	uint16_t bmHint;
	uint8_t  bFormatIndex;
	uint8_t  bFrameIndex;
	uint32_t dwFrameInterval;
	uint16_t wKeyFrameRate;
	uint16_t wPFrameRate;
	uint16_t wCompQuality;
	uint16_t wCompWindowSize;
	uint16_t wDelay;
	uint32_t dwMaxVideoFrameSize;
	uint32_t dwMaxPayloadTransferSize;
	uint32_t dwClockFrequency;
	uint8_t  bmFramingInfo;
	uint8_t  bPreferedVersion;
	uint8_t  bMinVersion;
	uint8_t  bMaxVersion;
	uint8_t  bInterfaceNumber;  /* UVC 1.5: interface to stream on */
} __attribute__((packed));

/* ── Global state ─────────────────────────────────────────────────────── */

static struct uvc_device g_uvc_devs[UVC_MAX_DEVICES];
static int g_uvc_count = 0;
static int g_initialized = 0;

/* ── Low-level UVC control request helper ─────────────────────────────── */

/*
 * Send a UVC class-specific control request to a video entity.
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @entity_id:   Unit/Terminal entity ID
 * @selector:    Control selector (PU_BRIGHTNESS, VS_PROBE_CONTROL, etc.)
 * @request:     Request code (UVC_REQ_SET_CUR or UVC_REQ_GET_CUR)
 * @data:        Pointer to data buffer
 * @len:         Length of data buffer in bytes
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_control_request(uint8_t dev_addr, uint8_t iface,
			       uint8_t entity_id, uint8_t selector,
			       uint8_t request,
			       void *data, uint16_t len)
{
	uint8_t bmReqType;

	if (request & 0x80)
		bmReqType = UVC_REQTYPE_GET_CUR;
	else
		bmReqType = UVC_REQTYPE_SET_CUR;

	uint16_t wValue = ((uint16_t)selector << 8) | (uint16_t)entity_id;
	uint16_t wIndex = (uint16_t)iface;

	return usb_control_msg(dev_addr, bmReqType, request,
			       wValue, wIndex, len, data);
}

/* ── Video probe/commit helpers ───────────────────────────────────────── */

/*
 * Send a video streaming probe or commit control request.
 * These are sent to the VideoStreaming interface, not the VideoControl
 * interface, with entity_id = 0 (interface-level control).
 *
 * @dev_addr:    USB device address
 * @vs_iface:    VideoStreaming interface number
 * @request:     UVC_REQ_SET_CUR or UVC_REQ_GET_CUR (for probe/commit)
 * @is_commit:   0 = probe, 1 = commit
 * @data:        Pointer to probe/commit buffer (26 bytes for UVC 1.5)
 * @len:         Length of data buffer
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_probe_commit_request(uint8_t dev_addr, uint8_t vs_iface,
				    uint8_t request, int is_commit,
				    void *data, uint16_t len)
{
	uint8_t selector = is_commit ? VS_COMMIT_CONTROL : VS_PROBE_CONTROL;

	return uvc_control_request(dev_addr, vs_iface, 0, selector,
				   request, data, len);
}

/*
 * Perform a video probe transaction:
 *   GET_CUR(PROBE) → SET_CUR(PROBE) → GET_CUR(PROBE)
 *
 * This validates the desired format/frame/interval against the device
 * and obtains the actual (possibly adjusted) parameters.
 *
 * @dev_addr:    USB device address
 * @vs_iface:    VideoStreaming interface number
 * @probe:       On entry: desired parameters; on exit: actual parameters
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_probe_transaction(uint8_t dev_addr, uint8_t vs_iface,
				 struct uvc_probe_commit *probe)
{
	int ret;
	uint16_t len = sizeof(struct uvc_probe_commit);

	/* Step 1: GET CUR(PROBE) to initialise probe structure */
	ret = uvc_probe_commit_request(dev_addr, vs_iface,
				       UVC_REQ_GET_CUR, 0,
				       probe, len);
	if (ret < 0)
		return ret;

	/* Step 2: SET CUR(PROBE) with desired parameters */
	ret = uvc_probe_commit_request(dev_addr, vs_iface,
				       UVC_REQ_SET_CUR, 0,
				       probe, len);
	if (ret < 0)
		return ret;

	/* Step 3: GET CUR(PROBE) to read back actual parameters */
	ret = uvc_probe_commit_request(dev_addr, vs_iface,
				       UVC_REQ_GET_CUR, 0,
				       probe, len);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Commit a video stream — activates the negotiated parameters.
 *
 * @dev_addr:    USB device address
 * @vs_iface:    VideoStreaming interface number
 * @commit:      Parameters to commit (usually after successful probe)
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_commit_stream(uint8_t dev_addr, uint8_t vs_iface,
			     const struct uvc_probe_commit *commit)
{
	return uvc_probe_commit_request(dev_addr, vs_iface,
					UVC_REQ_SET_CUR, 1,
					(void *)commit,
					sizeof(struct uvc_probe_commit));
}

/*
 * Start or stop video streaming by selecting the video streaming
 * interface's alternate setting. Setting 0 disables the stream.
 *
 * @dev_addr:    USB device address
 * @vs_iface:    VideoStreaming interface number
 * @alt_setting: Alternate setting (0 = stop, non-zero = start with that setting)
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_set_interface_alt_setting(uint8_t dev_addr, uint8_t vs_iface,
					 uint8_t alt_setting)
{
	return usb_control_msg(dev_addr,
			       USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_INTERFACE,
			       USB_REQ_SET_INTERFACE,
			       (uint16_t)alt_setting,
			       (uint16_t)vs_iface,
			       0, NULL);
}

/*
 * Start video streaming: commit the stream, then select the streaming
 * alternate setting to enable isochronous data delivery.
 *
 * @dev:  Video device instance
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_start_streaming(struct uvc_device *dev)
{
	int ret;

	if (dev->streaming_active)
		return 0;

	/* Commit the currently configured stream parameters */
	ret = uvc_commit_stream(dev->dev_addr, dev->vs_iface,
				(struct uvc_probe_commit *)dev->commit_buf);
	if (ret < 0) {
		kprintf("[usb_video] stream commit failed: %d\n", ret);
		return ret;
	}

	/* Select streaming alternate setting to enable isochronous IN endpoint */
	ret = uvc_set_interface_alt_setting(dev->dev_addr, dev->vs_iface,
					    dev->alt_setting);
	if (ret < 0) {
		kprintf("[usb_video] set alt setting failed: %d\n", ret);
		return ret;
	}

	dev->streaming_active = 1;
	kprintf("[usb_video] streaming started on dev %u iface %u alt %u\n",
		dev->dev_addr, dev->vs_iface, dev->alt_setting);

	return 0;
}

/*
 * Stop video streaming: select alternate setting 0 to disable
 * the isochronous endpoint.
 *
 * @dev:  Video device instance
 *
 * Returns 0 on success, negative errno on failure.
 */
static int uvc_stop_streaming(struct uvc_device *dev)
{
	int ret;

	if (!dev->streaming_active)
		return 0;

	ret = uvc_set_interface_alt_setting(dev->dev_addr, dev->vs_iface, 0);
	if (ret < 0) {
		kprintf("[usb_video] stop stream failed: %d\n", ret);
		return ret;
	}

	dev->streaming_active = 0;
	kprintf("[usb_video] streaming stopped on dev %u\n", dev->dev_addr);

	return 0;
}

/*
 * Capture a single isochronous video frame.
 *
 * Reads from the isochronous IN endpoint in a loop until one complete
 * frame is received. A frame is delimited by the USB packet header
 * (FID bit toggling indicates a new frame).
 *
 * @dev:   Video device instance
 * @buf:   Destination buffer for frame data
 * @size:  Size of destination buffer
 * @out_bytes:  Receives number of bytes actually received
 *
 * Returns 0 on success, negative errno on failure or timeout.
 */
static int uvc_capture_frame(struct uvc_device *dev,
			     uint8_t *buf, uint32_t size,
			     uint32_t *out_bytes)
{
	uint32_t total = 0;
	int ret;
	const int max_retries = 10000;

	if (!buf || !out_bytes || !dev->iso_in_ep)
		return -EINVAL;

	*out_bytes = 0;

	for (int i = 0; i < max_retries; i++) {
		uint8_t pkt_buf[USB_ISO_MAX_PACKET];
		uint32_t pkt_size = sizeof(pkt_buf);

		ret = usb_isochronous_msg(dev->dev_addr, dev->iso_in_ep,
					  pkt_buf, pkt_size, i & 0x3FF);
		if (ret < 0)
			return ret;

		/* Calculate actual payload (first 2 bytes are USB header) */
		/* In practice, usb_isochronous_msg may strip the header */
		uint32_t payload_len = ret;
		if (payload_len > sizeof(pkt_buf))
			payload_len = sizeof(pkt_buf);

		/* Copy payload to output buffer */
		uint32_t copy_len = payload_len;
		if (total + copy_len > size)
			copy_len = size - total;

		if (copy_len > 0) {
			memcpy(buf + total, pkt_buf, copy_len);
			total += copy_len;
		}

		/* End of frame: all isochronous packets for the current
		 * video frame have been received when we see the beginning
		 * of the next frame (FID toggle) or the frame data size
		 * matches the expected max_frame_size.
		 */
		if (total >= dev->max_frame_size) {
			*out_bytes = total;
			return 0;
		}

		/* Safety: if payload_len < expected header + data, frame
		 * might be done or we got a short packet end-of-frame marker.
		 * UVC ends a video frame with a short/zero-length packet.
		 */
		if (payload_len < 4) {
			*out_bytes = total;
			return 0;
		}
	}

	/* Timeout — partial frame is better than nothing */
	if (total > 0) {
		*out_bytes = total;
		return 0;
	}

	return -ETIMEDOUT;
}

/* ── UVC processing unit control helpers ──────────────────────────────── */

/*
 * Set a processing unit control value (integer).
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @unit_id:     Processing Unit entity ID (bUnitID)
 * @selector:    Control selector (PU_BRIGHTNESS, PU_CONTRAST, etc.)
 * @value:       Signed/unsigned 16-bit value to set
 *
 * Returns 0 on success, negative errno on failure.
 */
int uvc_set_pu_control(uint8_t dev_addr, uint8_t iface,
		       uint8_t unit_id, uint8_t selector,
		       int16_t value)
{
	return uvc_control_request(dev_addr, iface, unit_id, selector,
				   UVC_REQ_SET_CUR, &value, sizeof(value));
}

/*
 * Get a processing unit control value (integer).
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @unit_id:     Processing Unit entity ID
 * @selector:    Control selector
 * @out_value:   Receives the current value as signed 16-bit
 *
 * Returns 0 on success, negative errno on failure.
 */
int uvc_get_pu_control(uint8_t dev_addr, uint8_t iface,
		       uint8_t unit_id, uint8_t selector,
		       int16_t *out_value)
{
	int16_t val = 0;
	int ret;

	if (!out_value)
		return -EINVAL;

	ret = uvc_control_request(dev_addr, iface, unit_id, selector,
				  UVC_REQ_GET_CUR, &val, sizeof(val));
	if (ret < 0)
		return ret;

	*out_value = val;
	return 0;
}

/*
 * Get the range (min, max, step, default) of a processing unit control.
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @unit_id:     Processing Unit entity ID
 * @selector:    Control selector
 * @out_min:     Receives minimum value
 * @out_max:     Receives maximum value
 * @out_step:    Receives step size (resolution)
 * @out_def:     Receives default value
 *
 * Returns 0 on success, negative errno on failure.
 */
int uvc_get_pu_control_range(uint8_t dev_addr, uint8_t iface,
			     uint8_t unit_id, uint8_t selector,
			     int16_t *out_min, int16_t *out_max,
			     int16_t *out_step, int16_t *out_def)
{
	int16_t val;
	int ret;

	if (!out_min || !out_max || !out_step || !out_def)
		return -EINVAL;

	ret = uvc_control_request(dev_addr, iface, unit_id, selector,
				  UVC_REQ_GET_MIN, &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_min = val;

	ret = uvc_control_request(dev_addr, iface, unit_id, selector,
				  UVC_REQ_GET_MAX, &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_max = val;

	ret = uvc_control_request(dev_addr, iface, unit_id, selector,
				  UVC_REQ_GET_RES, &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_step = val;

	ret = uvc_control_request(dev_addr, iface, unit_id, selector,
				  UVC_REQ_GET_DEF, &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_def = val;

	return 0;
}

/* ── Camera terminal control helpers ──────────────────────────────────── */

/*
 * Set a camera terminal control value (32-bit, e.g. exposure time).
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @term_id:     Camera Terminal entity ID
 * @selector:    Control selector (CT_EXPOSURE_TIME, CT_ZOOM_ABSOLUTE, etc.)
 * @value:       Signed/unsigned 32-bit value to set
 *
 * Returns 0 on success, negative errno on failure.
 */
int uvc_set_ct_control_32(uint8_t dev_addr, uint8_t iface,
			  uint8_t term_id, uint8_t selector,
			  int32_t value)
{
	return uvc_control_request(dev_addr, iface, term_id, selector,
				   UVC_REQ_SET_CUR, &value, sizeof(value));
}

/*
 * Get a camera terminal control value (32-bit).
 *
 * @dev_addr:    USB device address
 * @iface:       VideoControl interface number
 * @term_id:     Camera Terminal entity ID
 * @selector:    Control selector
 * @out_value:   Receives the current value as signed 32-bit
 *
 * Returns 0 on success, negative errno on failure.
 */
int uvc_get_ct_control_32(uint8_t dev_addr, uint8_t iface,
			  uint8_t term_id, uint8_t selector,
			  int32_t *out_value)
{
	int32_t val = 0;
	int ret;

	if (!out_value)
		return -EINVAL;

	ret = uvc_control_request(dev_addr, iface, term_id, selector,
				  UVC_REQ_GET_CUR, &val, sizeof(val));
	if (ret < 0)
		return ret;

	*out_value = val;
	return 0;
}

/* ── Entity lookup helpers ────────────────────────────────────────────── */

static struct uvc_terminal *find_terminal_by_id(struct uvc_device *dev,
						uint8_t id)
{
	for (int i = 0; i < dev->num_terminals; i++) {
		if (dev->terminals[i].id == id)
			return &dev->terminals[i];
	}
	return NULL;
}

static struct uvc_processing_unit *find_processing_unit_by_id(
	struct uvc_device *dev, uint8_t id)
{
	for (int i = 0; i < dev->num_processing_units; i++) {
		if (dev->processing_units[i].id == id)
			return &dev->processing_units[i];
	}
	return NULL;
}

static struct uvc_format_desc *find_format_by_index(struct uvc_device *dev,
						    uint8_t index)
{
	for (int i = 0; i < dev->num_formats; i++) {
		if (dev->formats[i].format_index == index)
			return &dev->formats[i];
	}
	return NULL;
}

/* ── UVC 1.5 descriptor parsing ───────────────────────────────────────── */

/*
 * Parse a single UVC VideoControl descriptor.
 *
 * @dev:   Video device instance
 * @data:  Raw descriptor data (starting at bLength)
 * @length: Total descriptor length
 *
 * Returns 0 on success, negative errno on parse error.
 */
static int parse_uvc_control_desc(struct uvc_device *dev,
				  const uint8_t *data, uint8_t length)
{
	uint8_t subtype;

	if (!data || length < 3)
		return -EINVAL;

	subtype = data[2]; /* bDescriptorSubtype */

	switch (subtype) {
	case VC_HEADER: {
		/* VC_HEADER: variable length
		 * bLength(1) bDescType(1) bDescSubtype(1)
		 * bcdUVC(2) wTotalLength(2) dwClockFrequency(4)
		 * bmInCollection(1) baincollection[n]
		 */
		if (length < 12) {
			kprintf("[usb_video] short VC_HEADER (%u)\n", length);
			return -EINVAL;
		}

		uint16_t bcd_uvc = *(const uint16_t *)(data + 3);
		if (bcd_uvc >= 0x0150)
			dev->uvc_version = UVC_VERSION_1_5;
		else if (bcd_uvc >= 0x0110)
			dev->uvc_version = UVC_VERSION_1_1;
		else
			dev->uvc_version = UVC_VERSION_1_0;

		uint32_t clock_freq = *(const uint32_t *)(data + 7);

		kprintf("[usb_video] VC_HEADER: bcdUVC=0x%04X clock=%u Hz\n",
			bcd_uvc, clock_freq);
		return 0;
	}

	case VC_INPUT_TERMINAL: {
		/* Input Terminal: variable length
		 * bLength(1) bDescType(1) bDescSubtype(1)
		 * bTerminalID(1) wTerminalType(2) bAssocTerminal(1)
		 * iTerminal(1) ... optional camera controls
		 */
		if (length < 8) {
			kprintf("[usb_video] short input terminal (%u)\n", length);
			return -EINVAL;
		}
		if (dev->num_terminals >= UVC_MAX_TERMINALS) {
			kprintf("[usb_video] too many terminals\n");
			return -ENOSPC;
		}

		struct uvc_terminal *t = &dev->terminals[dev->num_terminals];
		memset(t, 0, sizeof(*t));
		t->id = data[3];
		t->terminal_type = *(const uint16_t *)(data + 4);
		t->assoc_terminal = data[6];
		t->is_input = 1;

		/* Parse camera-specific controls if present */
		if (t->terminal_type == VC_INPUT_CAMERA && length >= 15) {
			t->focal_min = *(const uint16_t *)(data + 8);
			t->focal_max = *(const uint16_t *)(data + 10);
			t->ocal_focal = *(const uint16_t *)(data + 12);
			t->control_size = data[14];
			if (length >= 15 + t->control_size && t->control_size <= 4) {
				uint32_t ctrl = 0;
				memcpy(&ctrl, data + 15, t->control_size);
				t->controls = ctrl;
			}
		}

		kprintf("[usb_video] Input Terminal id=%u type=0x%04X\n",
			t->id, t->terminal_type);

		dev->num_terminals++;
		return 0;
	}

	case VC_OUTPUT_TERMINAL: {
		/* Output Terminal
		 * bLength(1) bDescType(1) bDescSubtype(1)
		 * bTerminalID(1) wTerminalType(2) bAssocTerminal(1)
		 * bSourceID(1) iTerminal(1)
		 */
		if (length < 9) {
			kprintf("[usb_video] short output terminal (%u)\n", length);
			return -EINVAL;
		}
		if (dev->num_terminals >= UVC_MAX_TERMINALS) {
			kprintf("[usb_video] too many terminals\n");
			return -ENOSPC;
		}

		struct uvc_terminal *t = &dev->terminals[dev->num_terminals];
		memset(t, 0, sizeof(*t));
		t->id = data[3];
		t->terminal_type = *(const uint16_t *)(data + 4);
		t->assoc_terminal = data[6];
		t->is_input = 0;

		uint8_t src_id = data[7];
		(void)src_id;

		kprintf("[usb_video] Output Terminal id=%u type=0x%04X source=%u\n",
			t->id, t->terminal_type, src_id);

		dev->num_terminals++;
		return 0;
	}

	case VC_PROCESSING_UNIT: {
		/* Processing Unit: variable length
		 * bLength(1) bDescType(1) bDescSubtype(1)
		 * bUnitID(1) bSourceID(1) dwMaxMultiplier(4)
		 * bControlSize(1) bmControls[n] iProcessing(1)
		 */
		if (length < 10) {
			kprintf("[usb_video] short processing unit (%u)\n", length);
			return -EINVAL;
		}
		if (dev->num_processing_units >= UVC_MAX_UNITS) {
			kprintf("[usb_video] too many processing units\n");
			return -ENOSPC;
		}

		struct uvc_processing_unit *pu =
			&dev->processing_units[dev->num_processing_units];
		memset(pu, 0, sizeof(*pu));
		pu->id = data[3];
		pu->source_id = data[4];
		pu->max_multiplier = *(const uint16_t *)(data + 5);
		pu->control_size = data[9];

		if (length >= 10 + pu->control_size && pu->control_size <= 4) {
			uint32_t ctrl = 0;
			memcpy(&ctrl, data + 10, pu->control_size);
			pu->controls = ctrl;
		}

		pu->unit_index = data[length - 1];

		kprintf("[usb_video] Processing Unit id=%u source=%u "
			"ctrl_sz=%u ctrl=0x%08X\n",
			pu->id, pu->source_id,
			pu->control_size, pu->controls);

		dev->num_processing_units++;
		return 0;
	}

	case VC_EXTENSION_UNIT: {
		/* Extension Unit: variable length
		 * bLength(1) bDescType(1) bDescSubtype(1)
		 * bUnitID(1) bSourceID(1) guidExtensionCode(16)
		 * bNumControls(1) bNrInPins(1) baSourceID[n]
		 * bControlSize(1) bmControls[m] iExtension(1)
		 */
		if (length < 24) {
			kprintf("[usb_video] short extension unit (%u)\n", length);
			return -EINVAL;
		}
		if (dev->num_extension_units >= UVC_MAX_UNITS) {
			kprintf("[usb_video] too many extension units\n");
			return -ENOSPC;
		}

		struct uvc_extension_unit *eu =
			&dev->extension_units[dev->num_extension_units];
		memset(eu, 0, sizeof(*eu));
		eu->id = data[3];
		eu->source_id = data[4];

		/* Copy GUID (16 bytes starting at offset 5) */
		memcpy(eu->guid, data + 5, 16);

		eu->num_controls = data[21];

		uint8_t nr_in_pins = data[22];
		uint8_t pins_offset = 23 + nr_in_pins;

		if (pins_offset < length) {
			eu->control_size = data[pins_offset];
			if (pins_offset + 1 + eu->control_size <= length &&
			    eu->control_size <= 4) {
				uint32_t ctrl = 0;
				memcpy(&ctrl, data + pins_offset + 1,
				       eu->control_size);
				eu->controls = ctrl;
			}
		}

		eu->unit_index = data[length - 1];

		kprintf("[usb_video] Extension Unit id=%u source=%u "
			"num_ctrl=%u ctrl_sz=%u\n",
			eu->id, eu->source_id,
			eu->num_controls, eu->control_size);

		dev->num_extension_units++;
		return 0;
	}

	case VC_SELECTOR_UNIT:
		/* Selector Unit: skip for now — UVC 1.5 rarely uses
		 * selectors in practice; probe checks will still work.
		 */
		return 0;

	case VC_ENCODER_UNIT:
	case VC_DECODER_UNIT:
		/* UVC 1.5 encoder/decoder units */
		kprintf("[usb_video] Encoder/Decoder Unit (subtype=%u) "
			"not implemented\n", subtype);
		return 0;

	default:
		kprintf("[usb_video] unknown VC descriptor subtype %u\n",
			subtype);
		return 0;
	}
}

/*
 * Parse a single UVC VideoStreaming format descriptor.
 *
 * @dev:   Video device instance
 * @data:  Raw descriptor data
 * @length: Total descriptor length
 *
 * Returns 0 on success, negative errno on parse error.
 */
static int parse_uvc_format_desc(struct uvc_device *dev,
				 const uint8_t *data, uint8_t length)
{
	if (!data || length < 3)
		return -EINVAL;

	uint8_t subtype = data[2];

	/* Determine if this is a format descriptor we recognise */
	int is_format = 0;

	switch (subtype) {
	case VS_FORMAT_UNCOMPRESSED:
	case VS_FORMAT_MJPEG:
	case VS_FORMAT_FRAME_BASED:
	case VS_FORMAT_H264:
	case VS_FORMAT_VP8:
	case VS_FORMAT_MPEG2TS:
	case VS_FORMAT_DV:
	case VS_FORMAT_STREAM_BASED:
		is_format = 1;
		break;
	default:
		return 0;
	}

	if (!is_format)
		return 0;

	if (dev->num_formats >= UVC_MAX_FORMATS) {
		kprintf("[usb_video] too many formats\n");
		return -ENOSPC;
	}

	struct uvc_format_desc *fmt = &dev->formats[dev->num_formats];
	memset(fmt, 0, sizeof(*fmt));

	/* All UVC format descriptors share this common prefix:
	 * bLength(1) bDescType(1) bDescSubtype(1)
	 * bFormatIndex(1) bNumFrameDescriptors(1) guidFormat[16]
	 * bBitsPerPixel(1) bDefaultFrameIndex(1)
	 * bAspectRatioX(1) bAspectRatioY(1)
	 * bmInterlaceFlags(1) bCopyProtect(1)
	 */
	if (length < 27) {
		kprintf("[usb_video] short format descriptor (%u)\n", length);
		return -EINVAL;
	}

	fmt->format_index = data[3];
	fmt->num_frame_descs = data[4];
	memcpy(fmt->guid, data + 5, 16);
	fmt->bits_per_pixel = data[21];
	fmt->default_frame_index = data[22];
	fmt->aspect_ratio_x = data[23];
	fmt->aspect_ratio_y = data[24];
	fmt->interlace_flags = data[25];
	fmt->copy_protect = data[26];

	/* UVC 1.5 frame-based formats have a bVariableSize field at offset 27 */
	if (length >= 28 && (subtype == VS_FORMAT_FRAME_BASED ||
			     subtype == VS_FORMAT_H264 ||
			     subtype == VS_FORMAT_VP8)) {
		fmt->variable_size = data[27];

		/* UVC 1.5 extended colorimetry at offset 28-30 */
		if (length >= 31) {
			fmt->color_primaries = data[28];
			fmt->transfer_chars = data[29];
			fmt->matrix_coeff = data[30];
		}
	}

	fmt->num_frames = 0;

	kprintf("[usb_video] Format %u: type=0x%02X idx=%u bpp=%u "
		"frames=%u\n",
		fmt->format_index, subtype, fmt->format_index,
		fmt->bits_per_pixel, fmt->num_frame_descs);

	dev->num_formats++;
	return 0;
}

/*
 * Parse a single UVC VideoStreaming frame descriptor.
 *
 * @dev:   Video device instance
 * @data:  Raw descriptor data
 * @length: Total descriptor length
 *
 * Returns 0 on success, negative errno on parse error.
 */
static int parse_uvc_frame_desc(struct uvc_device *dev,
				const uint8_t *data, uint8_t length)
{
	if (!data || length < 3)
		return -EINVAL;

	uint8_t subtype = data[2];

	/* Only process frame descriptors (for uncompressed, MJPEG,
	 * frame-based, H.264, or VP8).
	 */
	if (subtype != VS_FRAME_UNCOMPRESSED &&
	    subtype != VS_FRAME_MJPEG &&
	    subtype != VS_FRAME_FRAME_BASED &&
	    subtype != VS_FRAME_H264 &&
	    subtype != VS_FRAME_VP8) {
		return 0;
	}

	if (dev->num_formats == 0)
		return -EINVAL;

	struct uvc_format_desc *fmt = &dev->formats[dev->num_formats - 1];

	if (fmt->num_frames >= UVC_MAX_FRAMES) {
		kprintf("[usb_video] too many frames for format %u\n",
			fmt->format_index);
		return -ENOSPC;
	}

	/* Common frame descriptor prefix (26 bytes for UVC 1.5):
	 * bLength(1) bDescType(1) bDescSubtype(1)
	 * bFrameIndex(1) bmCapabilities(1)
	 * wWidth(2) wHeight(2) dwMinBitRate(4) dwMaxBitRate(4)
	 * dwMaxVideoFrameBufferSize(4) dwDefaultFrameInterval(4)
	 * bFrameIntervalType(1) <intervals>
	 */
	if (length < 26) {
		kprintf("[usb_video] short frame descriptor (%u)\n", length);
		return -EINVAL;
	}

	struct uvc_frame_desc *fr = &fmt->frames[fmt->num_frames];
	memset(fr, 0, sizeof(*fr));

	fr->width = *(const uint16_t *)(data + 5);
	fr->height = *(const uint16_t *)(data + 7);
	fr->min_bit_rate = *(const uint32_t *)(data + 9);
	fr->max_bit_rate = *(const uint32_t *)(data + 13);
	fr->max_frame_size = *(const uint32_t *)(data + 17);
	fr->default_frame_interval = *(const uint32_t *)(data + 21);

	/* Update the device's estimated max frame size */
	if (fr->max_frame_size > dev->max_frame_size)
		dev->max_frame_size = fr->max_frame_size;

	fr->intervals_discrete = data[25];

	/* Parse frame intervals */
	if (fr->intervals_discrete == 0) {
		/* Continuous interval: three 4-byte values:
		 * dwMinFrameInterval, dwMaxFrameInterval, dwFrameIntervalStep
		 */
		if (length >= 38) {
			fr->min_interval = *(const uint32_t *)(data + 26);
			fr->max_interval = *(const uint32_t *)(data + 30);
			fr->interval_step = *(const uint32_t *)(data + 34);
		}
	} else {
		/* Discrete intervals: N × 4-byte values */
		fr->min_interval = *(const uint32_t *)(data + 26);
	}

	kprintf("[usb_video] Frame %u: %ux%u max_size=%u default_int=%u\n",
		data[3], fr->width, fr->height,
		fr->max_frame_size, fr->default_frame_interval);

	fmt->num_frames++;
	return 0;
}

/*
 * Parse a UVC 1.5 color matching descriptor (VS_COLOR_FORMAT).
 *
 * @dev:   Video device instance
 * @data:  Raw descriptor data
 * @length: Total descriptor length
 */
static int parse_uvc_color_matching(struct uvc_device *dev,
				    const uint8_t *data, uint8_t length)
{
	if (!data || length < sizeof(struct uvc_color_matching))
		return -EINVAL;

	if (dev->num_formats == 0)
		return -EINVAL;

	struct uvc_format_desc *fmt = &dev->formats[dev->num_formats - 1];

	const struct uvc_color_matching *cm =
		(const struct uvc_color_matching *)data;

	memcpy(&fmt->color_matching, cm, sizeof(*cm));

	kprintf("[usb_video] Color matching: prim=%u trans=%u matrix=%u\n",
		cm->bColorPrimaries, cm->bTransferCharacteristics,
		cm->bMatrixCoefficients);

	return 0;
}

/* ── Topology discovery ───────────────────────────────────────────────── */

/*
 * Print the video function topology for debugging.
 */
static void dump_uvc_topology(struct uvc_device *dev)
{
	kprintf("[usb_video] Video topology for dev %u:\n", dev->dev_addr);

	for (int i = 0; i < dev->num_terminals; i++) {
		struct uvc_terminal *t = &dev->terminals[i];
		kprintf("[usb_video]   %s Terminal id=%u type=0x%04X\n",
			t->is_input ? "Input" : "Output",
			t->id, t->terminal_type);
	}

	for (int i = 0; i < dev->num_processing_units; i++) {
		struct uvc_processing_unit *pu =
			&dev->processing_units[i];
		kprintf("[usb_video]   Processing Unit id=%u source=%u "
			"ctrl=0x%08X\n",
			pu->id, pu->source_id, pu->controls);
	}

	for (int i = 0; i < dev->num_extension_units; i++) {
		struct uvc_extension_unit *eu =
			&dev->extension_units[i];
		kprintf("[usb_video]   Extension Unit id=%u source=%u\n",
			eu->id, eu->source_id);
	}

	for (int i = 0; i < dev->num_formats; i++) {
		struct uvc_format_desc *fmt = &dev->formats[i];
		kprintf("[usb_video]   Format %u: %d frames\n",
			fmt->format_index, fmt->num_frames);
	}
}

/* ── Configuration descriptor walking ─────────────────────────────────── */

struct uvc_parse_ctx {
	struct uvc_device *dev;
	int    vc_iface_found;
	int    vs_iface_found;
	uint8_t current_iface;
	uint8_t current_iface_class;
	uint8_t current_iface_subclass;
	uint8_t current_iface_protocol;
	int    in_vs_header;   /* inside a VS_HEADER, expect format descs */
	int    in_format;       /* inside a format, expect frame descs */
};

static int uvc_desc_callback(uint8_t bDescriptorType,
			     const uint8_t *data, uint8_t bLength,
			     void *user_data)
{
	struct uvc_parse_ctx *ctx = (struct uvc_parse_ctx *)user_data;
	struct uvc_device *dev = ctx->dev;
	int ret = 0;

	if (bDescriptorType == USB_DT_INTERFACE) {
		const struct usb_interface_descriptor *iface =
			(const struct usb_interface_descriptor *)data;
		ctx->current_iface = iface->bInterfaceNumber;
		ctx->current_iface_class = iface->bInterfaceClass;
		ctx->current_iface_subclass = iface->bInterfaceSubClass;
		ctx->current_iface_protocol = iface->bInterfaceProtocol;
		ctx->in_vs_header = 0;
		ctx->in_format = 0;

		/* Track video control interface */
		if (iface->bInterfaceClass == USB_CLASS_VIDEO &&
		    iface->bInterfaceSubClass == UVC_SUBCLASS_CONTROL) {
			ctx->vc_iface_found = 1;
			dev->vc_iface = iface->bInterfaceNumber;
		}

		/* Track video streaming interface (pick first) */
		if (iface->bInterfaceClass == USB_CLASS_VIDEO &&
		    iface->bInterfaceSubClass == UVC_SUBCLASS_STREAMING) {
			if (!ctx->vs_iface_found) {
				ctx->vs_iface_found = 1;
				dev->vs_iface = iface->bInterfaceNumber;
				dev->alt_setting = iface->bAlternateSetting;
			}
		}
		return 0;
	}

	/* Class-specific video descriptors (CS_INTERFACE: 0x24) */
	if (bDescriptorType == CS_INTERFACE) {
		if (ctx->current_iface_class != USB_CLASS_VIDEO)
			return 0;

		if (ctx->current_iface_subclass == UVC_SUBCLASS_CONTROL) {
			ret = parse_uvc_control_desc(dev, data, bLength);
		} else if (ctx->current_iface_subclass == UVC_SUBCLASS_STREAMING) {
			if (bLength >= 3) {
				uint8_t subtype = data[2];

				/* Track streaming header vs format vs frame context */
				if (subtype == VS_INPUT_HEADER ||
				    subtype == VS_OUTPUT_HEADER) {
					ctx->in_vs_header = 1;
					ctx->in_format = 0;

					/* Parse VS_INPUT_HEADER for still capture support */
					if (subtype == VS_INPUT_HEADER && bLength >= 13) {
						dev->bStillCaptureMethod = data[8];
						dev->bTriggerSupport = data[9];
						dev->bTriggerUsage = data[10];
					}
				} else if (subtype == VS_FORMAT_UNCOMPRESSED ||
					   subtype == VS_FORMAT_MJPEG ||
					   subtype == VS_FORMAT_FRAME_BASED ||
					   subtype == VS_FORMAT_H264 ||
					   subtype == VS_FORMAT_VP8 ||
					   subtype == VS_FORMAT_MPEG2TS ||
					   subtype == VS_FORMAT_DV ||
					   subtype == VS_FORMAT_STREAM_BASED) {
					ctx->in_format = 1;
					ctx->in_vs_header = 0;
					ret = parse_uvc_format_desc(dev, data, bLength);
				} else if (subtype == VS_FRAME_UNCOMPRESSED ||
					   subtype == VS_FRAME_MJPEG ||
					   subtype == VS_FRAME_FRAME_BASED ||
					   subtype == VS_FRAME_H264 ||
					   subtype == VS_FRAME_VP8) {
					if (ctx->in_format) {
						ret = parse_uvc_frame_desc(dev, data, bLength);
					}
				} else if (subtype == VS_COLOR_FORMAT) {
					if (ctx->in_format) {
						ret = parse_uvc_color_matching(dev, data, bLength);
					}
				} else if (subtype == VS_STILL_IMAGE_FRAME) {
					/* Still image frame descriptor: skip payload-specific */
					return 0;
				}
			}
		}
		return ret;
	}

	/* Endpoint descriptors within video streaming interfaces */
	if (bDescriptorType == USB_DT_ENDPOINT &&
	    ctx->current_iface_class == USB_CLASS_VIDEO &&
	    ctx->current_iface_subclass == UVC_SUBCLASS_STREAMING) {
		const struct usb_endpoint_descriptor *ep =
			(const struct usb_endpoint_descriptor *)data;
		uint8_t ep_addr = ep->bEndpointAddress;
		uint8_t ep_attr = ep->bmAttributes;

		if ((ep_attr & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_ISOCH)
			return 0;

		uint16_t max_pkt = ep->wMaxPacketSize;

		if (ep_addr & USB_ENDPOINT_DIR_IN) {
			dev->iso_in_ep = ep_addr;
			kprintf("[usb_video]   ISO IN ep 0x%02X max=%u\n",
				ep_addr, max_pkt);
		} else {
			dev->iso_out_ep = ep_addr;
			kprintf("[usb_video]   ISO OUT ep 0x%02X max=%u\n",
				ep_addr, max_pkt);
		}

		if (max_pkt > dev->max_packet_size)
			dev->max_packet_size = max_pkt;

		return 0;
	}

	/* Class-specific endpoint descriptors (CS_ENDPOINT: 0x25) */
	if (bDescriptorType == CS_ENDPOINT &&
	    ctx->current_iface_class == USB_CLASS_VIDEO &&
	    ctx->current_iface_subclass == UVC_SUBCLASS_STREAMING) {
		/* Skip CS_ENDPOINT descriptors (EP_INTERRUPT, EP_GENERAL) */
		return 0;
	}

	return 0;
}

/* ── Probe ────────────────────────────────────────────────────────────── */

/*
 * Probe a USB video device — parse descriptors, discover topology.
 *
 * @dev_desc:    USB device descriptor from the core
 *
 * Returns 0 on success (device is video), negative errno on failure.
 */
static int usb_video_probe(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return -EINVAL;

	/* Only match video class devices */
	if (dev_desc->class_code != USB_CLASS_VIDEO &&
	    dev_desc->class_code != 0) {
		return -ENODEV;
	}

	/* Allocate a video device slot */
	if (g_uvc_count >= UVC_MAX_DEVICES) {
		kprintf("[usb_video] too many video devices\n");
		return -ENOSPC;
	}

	struct uvc_device *dev = &g_uvc_devs[g_uvc_count];
	memset(dev, 0, sizeof(struct uvc_device));
	dev->dev_addr = dev_desc->addr;

	/* Try to read the configuration descriptor to discover topology */
	uint8_t config_buf[256];
	int ret;

	memset(config_buf, 0, sizeof(config_buf));

	/* Read config descriptor header first to get wTotalLength */
	ret = usb_control_msg(dev_desc->addr,
			      USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      (USB_DT_CONFIG << 8) | 0,
			      0, 4, config_buf);
	if (ret < 0) {
		kprintf("[usb_video] failed to read config header: %d\n", ret);
		/* Non-fatal — we can still register the device */
		dev->active = 1;
		g_uvc_count++;
		return 0;
	}

	uint16_t total_len = *(const uint16_t *)(config_buf + 2);
	if (total_len > sizeof(config_buf)) {
		kprintf("[usb_video] config too large (%u), truncating\n",
			total_len);
		total_len = sizeof(config_buf);
	}

	/* Read full configuration descriptor */
	ret = usb_control_msg(dev_desc->addr,
			      USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      (USB_DT_CONFIG << 8) | 0,
			      0, total_len, config_buf);
	if (ret < 0) {
		kprintf("[usb_video] failed to read full config: %d\n", ret);
		dev->active = 1;
		g_uvc_count++;
		return 0;
	}

	/* Walk sub-descriptors to discover video topology */
	struct uvc_parse_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dev = dev;

	ret = usb_for_each_config_subdesc(config_buf, total_len,
					  uvc_desc_callback, &ctx);
	if (ret < 0 && ret != -ENOENT) {
		kprintf("[usb_video] descriptor walk error: %d\n", ret);
	}

	/* Check if we found any video interfaces */
	if (!ctx.vc_iface_found && !ctx.vs_iface_found) {
		kprintf("[usb_video] no video interfaces found for dev %u\n",
			dev_desc->addr);
		return -ENODEV;
	}

	/* Allocate frame buffer if max frame size is known */
	if (dev->max_frame_size > 0 && dev->max_frame_size <= UVC_MAX_FRAME_SIZE) {
		dev->frame_buffer = (uint8_t *)kmalloc(dev->max_frame_size);
		if (dev->frame_buffer) {
			dev->frame_buffer_size = dev->max_frame_size;
		}
	}

	/* Dump discovered topology */
	dump_uvc_topology(dev);

	dev->active = 1;
	g_uvc_count++;

	kprintf("[usb_video] UVC video device %u probed: "
		"%d terminals, %d processing units, %d extension units, "
		"%d formats\n",
		dev_desc->addr,
		dev->num_terminals, dev->num_processing_units,
		dev->num_extension_units, dev->num_formats);

	return 0;
}

/* ── Disconnect ───────────────────────────────────────────────────────── */

static void usb_video_disconnect(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return;

	/* Find and deactivate the video device */
	for (int i = 0; i < g_uvc_count; i++) {
		if (g_uvc_devs[i].dev_addr == dev_desc->addr &&
		    g_uvc_devs[i].active) {
			/* Stop streaming if active */
			if (g_uvc_devs[i].streaming_active)
				uvc_stop_streaming(&g_uvc_devs[i]);

			/* Free frame buffer */
			if (g_uvc_devs[i].frame_buffer) {
				kfree(g_uvc_devs[i].frame_buffer);
				g_uvc_devs[i].frame_buffer = NULL;
				g_uvc_devs[i].frame_buffer_size = 0;
			}

			g_uvc_devs[i].active = 0;
			kprintf("[usb_video] device %u disconnected\n",
				dev_desc->addr);
			break;
		}
	}
}

/* ── USB device ID table ─────────────────────────────────────────────── */

static const struct usb_device_id usb_video_ids[] = {
	/* Match all USB Video Class 1.0/1.1/1.5 devices by class */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			 USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			 USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_VIDEO, .subclass = 0x00, .protocol = UVC_PROTOCOL_1_0 },
	/* Match USB Video Class 1.5 devices (protocol = 0x01) */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			 USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			 USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_VIDEO, .subclass = 0x00, .protocol = UVC_PROTOCOL_1_5 },
	/* Also match devices that specify video class at interface level
	 * (device class = 0, subclass = 0, protocol = 0 — interface decides).
	 */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
	  .class = 0x00, .subclass = 0x00, .protocol = 0x00 },
	USB_DEVICE_TABLE_END
};

/* ── Driver registration ─────────────────────────────────────────────── */

static struct usb_driver g_usb_video_driver = {
	.name       = "usb_video",
	.id_table   = usb_video_ids,
	.probe      = usb_video_probe,
	.disconnect = usb_video_disconnect,
};

void __init usb_video_init(void)
{
	if (g_initialized)
		return;

	memset(g_uvc_devs, 0, sizeof(g_uvc_devs));
	g_uvc_count = 0;
	g_initialized = 1;

	usb_register_driver(&g_usb_video_driver);

	kprintf("[usb_video] USB Video Class 1.5 (UVC) driver registered\n");
}

void usb_video_exit(void)
{
	usb_deregister_driver(&g_usb_video_driver);

	/* Deactivate all devices and free frame buffers */
	for (int i = 0; i < g_uvc_count; i++) {
		if (g_uvc_devs[i].streaming_active)
			uvc_stop_streaming(&g_uvc_devs[i]);

		if (g_uvc_devs[i].frame_buffer) {
			kfree(g_uvc_devs[i].frame_buffer);
			g_uvc_devs[i].frame_buffer = NULL;
			g_uvc_devs[i].frame_buffer_size = 0;
		}

		g_uvc_devs[i].active = 0;
	}

	g_uvc_count = 0;
	g_initialized = 0;

	kprintf("[usb_video] USB Video Class 1.5 driver unregistered\n");
}

module_init(usb_video_init);
module_exit(usb_video_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.5.0");
MODULE_DESCRIPTION("USB Video Class 1.5 (UVC) driver with terminal/unit "
		   "topology, format/frame descriptors, probe/commit "
		   "control, and isochronous frame capture");
MODULE_AUTHOR("OS Kernel Team");
MODULE_ALIAS("usb:v* p* d* dc0E dsc* dp*");          /* USB Video Class (UVC 1.0) */
MODULE_ALIAS("usb:v* p* d* dc0E dsc* dp01");          /* USB Video Class (UVC 1.5 protocol 0x01) */
