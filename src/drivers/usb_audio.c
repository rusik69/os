/*
 * usb_audio.c — USB Audio Class 1.0 (UAC1) driver
 *
 * Implements USB Audio Class 1.0 feature unit controls (mute, volume,
 * tone) and audio streaming interface detection.  Parses the AudioControl
 * interface descriptors to build a topology of terminals and feature units,
 * then exposes class-specific requests for volume/mute control.
 *
 * References:
 *   USB Device Class Definition for Audio Devices, Release 1.0 (UAC1)
 *   USB Audio Data Formats, Release 1.0
 *   USB Audio Terminals Types, Release 1.0
 *   EHCI Specification, Revision 1.0 (isochronous transfer support)
 */

#define KERNEL_INTERNAL
#include "usb.h"
#include "usb_core.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "pmm.h"
#include "module.h"
#include "types.h"

/* ── UAC1 class constants ────────────────────────────────────────────── */

#define UAC_VERSION_1_0          0x0100

/* Audio interface subclass codes */
#define AUDIO_SUBCLASS_UNDEFINED 0x00
#define AUDIO_SUBCLASS_CONTROL   0x01   /* AudioControl interface */
#define AUDIO_SUBCLASS_STREAMING 0x02   /* AudioStreaming interface */
#define AUDIO_SUBCLASS_MIDI      0x03   /* MIDIStreaming interface */

/* Audio interface protocol codes */
#define AUDIO_PROTOCOL_UNDEFINED 0x00
#define AUDIO_PROTOCOL_V1_0      0x00   /* UAC1 uses protocol = 0x00 */

/* ── AudioControl descriptor subtypes (CS_INTERFACE) ──────────────────── */
/* USB class-specific descriptor type for audio interfaces */
#define CS_INTERFACE             0x24
#define CS_ENDPOINT              0x25

/* AudioControl interface descriptor subtypes */
#define AC_HEADER                0x01
#define AC_INPUT_TERMINAL        0x02
#define AC_OUTPUT_TERMINAL       0x03
#define AC_MIXER_UNIT            0x04
#define AC_SELECTOR_UNIT         0x05
#define AC_FEATURE_UNIT          0x06
#define AC_PROCESSING_UNIT       0x07
#define AC_EXTENSION_UNIT        0x08

/* ── AudioStreaming descriptor subtypes (CS_INTERFACE) ────────────────── */
#define AS_GENERAL               0x01
#define AS_FORMAT_TYPE           0x02
#define AS_FORMAT_SPECIFIC       0x03

/* ── AudioStreaming endpoint descriptor subtypes (CS_ENDPOINT) ────────── */
#define EP_GENERAL               0x01

/* ── Audio class-specific request codes ───────────────────────────────── */
#define AUDIO_REQ_SET_CUR        0x01
#define AUDIO_REQ_GET_CUR        0x81
#define AUDIO_REQ_SET_MIN        0x02
#define AUDIO_REQ_GET_MIN        0x82
#define AUDIO_REQ_SET_MAX        0x03
#define AUDIO_REQ_GET_MAX        0x83
#define AUDIO_REQ_SET_RES        0x04
#define AUDIO_REQ_GET_RES        0x84
#define AUDIO_REQ_SET_MEM        0x05
#define AUDIO_REQ_GET_MEM        0x85
#define AUDIO_REQ_GET_STAT       0xFF

/* ── Feature Unit Control Selectors (FU_CONTROL_SELECTOR) ─────────────── */
#define FU_CONTROL_UNDEFINED      0x00
#define MUTE_CONTROL              0x01
#define VOLUME_CONTROL            0x02
#define BASS_CONTROL              0x03
#define MID_CONTROL               0x04
#define TREBLE_CONTROL            0x05
#define GRAPHIC_EQUALIZER_CONTROL 0x06
#define AUTOMATIC_GAIN_CONTROL    0x07
#define DELAY_CONTROL             0x08
#define BASS_BOOST_CONTROL        0x09
#define LOUDNESS_CONTROL          0x0A

/* Request type values for audio class requests */
#define AUDIO_REQTYPE_SET_CUR    (USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)
#define AUDIO_REQTYPE_GET_CUR    (USB_DIR_IN  | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE)

/* ── Feature unit bmaControls bitmask flags (per UAC1) ────────────────── */
#define FU_CTRL_MUTE            (1U << 0)
#define FU_CTRL_VOLUME          (1U << 1)
#define FU_CTRL_BASS            (1U << 2)
#define FU_CTRL_MID             (1U << 3)
#define FU_CTRL_TREBLE          (1U << 4)
#define FU_CTRL_GRAPHIC_EQ      (1U << 5)
#define FU_CTRL_AUTOMATIC_GAIN  (1U << 6)
#define FU_CTRL_DELAY           (1U << 7)
#define FU_CTRL_BASS_BOOST      (1U << 8)
#define FU_CTRL_LOUDNESS        (1U << 9)

/* ── Audio terminal types (wTerminalType) ────────────────────────────── */
#define TERMINAL_UNDEFINED        0x0100
#define TERMINAL_STREAMING        0x0101
#define TERMINAL_VENDOR           0x01FF

#define TERMINAL_IN_UNDEFINED     0x0200
#define TERMINAL_IN_MICROPHONE    0x0201
#define TERMINAL_IN_DESKTOP_MIC   0x0202
#define TERMINAL_IN_PERSONAL_MIC  0x0203
#define TERMINAL_IN_OMNI_MIC      0x0204
#define TERMINAL_IN_MIC_ARRAY     0x0205
#define TERMINAL_IN_PROC_MIC_ARRAY 0x0206

#define TERMINAL_OUT_UNDEFINED    0x0300
#define TERMINAL_OUT_SPEAKER      0x0301
#define TERMINAL_OUT_HEADPHONES   0x0302
#define TERMINAL_OUT_HEAD_MOUNTED 0x0303
#define TERMINAL_OUT_DESKTOP      0x0304
#define TERMINAL_OUT_ROOM         0x0305
#define TERMINAL_OUT_COMMS        0x0306
#define TERMINAL_OUT_LF           0x0307

#define TERMINAL_BIDIR_UNDEFINED  0x0400
#define TERMINAL_BIDIR_HANDSET    0x0401
#define TERMINAL_BIDIR_HEADSET    0x0402
#define TERMINAL_BIDIR_SPEAKERPHONE 0x0403
#define TERMINAL_BIDIR_ECHO_SUPPRESS 0x0404
#define TERMINAL_BIDIR_ECHO_CANCEL 0x0405

/* ── Audio format type descriptors ───────────────────────────────────── */
#define FORMAT_TYPE_I             0x01   /* PCM */
#define FORMAT_TYPE_II            0x02   /* DoP / non-PCM */
#define FORMAT_TYPE_III           0x03   /* IEC61937 compressed */

/* PCM format type I bit depths */
#define FORMAT_I_PCM_8            (1U << 0)
#define FORMAT_I_PCM_16           (1U << 1)
#define FORMAT_I_PCM_24           (1U << 2)
#define FORMAT_I_PCM_32           (1U << 3)

/* ── limits ──────────────────────────────────────────────────────────── */

#define AUDIO_MAX_TERMINALS       8       /* max terminals per audio function */
#define AUDIO_MAX_FEATURE_UNITS   8       /* max feature units per audio function */
#define AUDIO_MAX_CHANNELS        8       /* max audio channels per feature unit */
#define AUDIO_MAX_SAMPLE_RATES    16      /* max sample rates per streaming iface */
#define AUDIO_MAX_DEVICES         4       /* max audio devices tracked */
#define AUDIO_MAX_FREQ            192000  /* max sample rate in Hz */

/* ── Audio control request structure ─────────────────────────────────── */
/*
 * Used for class-specific audio control requests to feature units:
 *   SET_CUR  / GET_CUR  (current value)
 *   SET_MIN  / GET_MIN  (minimum value)
 *   SET_MAX  / GET_MAX  (maximum value)
 *   SET_RES  / GET_RES  (resolution / step size)
 */
struct audio_control_req {
	uint8_t  bmRequestType;
	uint8_t  bRequest;
	uint16_t wValue;      /* (control_selector << 8) | entity_id */
	uint16_t wIndex;      /* interface number */
	uint16_t wLength;     /* data payload length */
};

/* ── Audio terminal (input or output) record ──────────────────────────── */

struct audio_terminal {
	uint8_t  id;              /* terminal ID */
	uint16_t terminal_type;   /* wTerminalType */
	uint8_t  is_input;        /* 1 = input terminal, 0 = output terminal */
	uint8_t  nr_channels;     /* bNrChannels */
	uint16_t channel_config;  /* wChannelConfig */
	uint8_t  assoc_terminal;  /* bAssocTerminal */
};

/* ── Feature unit record ─────────────────────────────────────────────── */

struct audio_feature_unit {
	uint8_t  id;              /* bUnitID */
	uint8_t  source_id;       /* bSourceID — entity feeding this FU */
	uint8_t  control_size;    /* bControlSize (1 for UAC1) */
	uint16_t master_controls; /* bmaControls(0) — master channel */
	uint16_t channel_controls[AUDIO_MAX_CHANNELS]; /* per-channel controls */
	uint8_t  num_channels;    /* number of logical channels */
	uint8_t  feature_index;   /* iFeature string descriptor index */
};

/* ── Audio device instance ────────────────────────────────────────────── */

struct audio_device {
	uint8_t  dev_addr;        /* USB device address */
	uint8_t  ac_iface;        /* AudioControl interface number */
	uint8_t  as_iface;        /* AudioStreaming interface number (first found) */
	uint8_t  alt_setting;     /* alternate setting for streaming */
	int      active;          /* 1 if device is initialised */

	/* Topology */
	int      num_terminals;
	struct audio_terminal terminals[AUDIO_MAX_TERMINALS];

	int      num_feature_units;
	struct audio_feature_unit feature_units[AUDIO_MAX_FEATURE_UNITS];

	/* Streaming info */
	uint8_t  iso_in_ep;       /* isochronous IN endpoint */
	uint8_t  iso_out_ep;      /* isochronous OUT endpoint */
	uint16_t max_packet_size;
	uint8_t  num_channels;
	uint8_t  bit_resolution;
	uint32_t sample_rates[AUDIO_MAX_SAMPLE_RATES];
	int      num_sample_rates;

	/* Cached control values */
	uint8_t  mute_state;      /* current mute (0 = not muted, 1 = muted) */
	int16_t  volume;          /* current volume in dB/256 units */
};

/* ── Global state ─────────────────────────────────────────────────────── */

static struct audio_device g_audio_devs[AUDIO_MAX_DEVICES];
static int g_audio_count = 0;
static int g_initialized = 0;

/* ── Low-level audio class control request helper ─────────────────────── */

/*
 * Send an audio class-specific control request to a feature unit.
 *
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @entity_id:   Feature Unit entity ID (bUnitID)
 * @selector:    Control selector (MUTE_CONTROL, VOLUME_CONTROL, ...)
 * @channel:     Channel number (0 = master, 1..n = per-channel)
 * @request:     Request code (AUDIO_REQ_SET_CUR or AUDIO_REQ_GET_CUR)
 * @data:        Pointer to data buffer
 * @len:         Length of data buffer in bytes
 *
 * Returns 0 on success, negative errno on failure.
 */
static int audio_feature_unit_request(uint8_t dev_addr, uint8_t iface,
				      uint8_t entity_id, uint8_t selector,
				      uint8_t channel, uint8_t request,
				      void *data, uint16_t len)
{
	uint8_t bmReqType;
	uint16_t wValue;

	if (request & 0x80)
		bmReqType = AUDIO_REQTYPE_GET_CUR;
	else
		bmReqType = AUDIO_REQTYPE_SET_CUR;

	/* CS = channel << 8, entity ID = low byte */
	wValue = ((uint16_t)selector << 8) | (uint16_t)entity_id;

	/* For per-channel controls, channel number is in wIndex high byte */
	uint16_t wIndex = (uint16_t)iface | ((uint16_t)channel << 8);

	return usb_control_msg(dev_addr, bmReqType, request,
			       wValue, wIndex, len, data);
}

/* ── Public feature unit control API ──────────────────────────────────── */

/*
 * Set the mute state of a feature unit.
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @fu_id:       Feature Unit entity ID
 * @muted:       1 to mute, 0 to unmute
 *
 * Returns 0 on success, negative errno on failure.
 */
int audio_set_mute(uint8_t dev_addr, uint8_t iface,
		   uint8_t fu_id, uint8_t muted)
{
	uint8_t val = muted ? 1 : 0;
	return audio_feature_unit_request(dev_addr, iface, fu_id,
					  MUTE_CONTROL, 0,
					  AUDIO_REQ_SET_CUR,
					  &val, sizeof(val));
}

/*
 * Get the mute state of a feature unit.
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @fu_id:       Feature Unit entity ID
 * @out_muted:   Receives mute state (1 = muted, 0 = unmuted)
 *
 * Returns 0 on success, negative errno on failure.
 */
int audio_get_mute(uint8_t dev_addr, uint8_t iface,
		   uint8_t fu_id, uint8_t *out_muted)
{
	uint8_t val = 0;
	int ret;

	if (!out_muted)
		return -EINVAL;

	ret = audio_feature_unit_request(dev_addr, iface, fu_id,
					 MUTE_CONTROL, 0,
					 AUDIO_REQ_GET_CUR,
					 &val, sizeof(val));
	if (ret < 0)
		return ret;

	*out_muted = val ? 1 : 0;
	return 0;
}

/*
 * Set the volume of a feature unit.
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @fu_id:       Feature Unit entity ID
 * @channel:     Channel number (0 = master, 1..n = per-channel)
 * @volume:      Volume in 1/256 dB units (signed 16-bit)
 *
 * UAC1 volume is expressed in signed 16-bit 1/256 dB resolution.
 * A value of 0 dBFS corresponds to volume = 0.
 * Negative values represent attenuation.
 *
 * Returns 0 on success, negative errno on failure.
 */
int audio_set_volume(uint8_t dev_addr, uint8_t iface,
		     uint8_t fu_id, uint8_t channel, int16_t volume)
{
	return audio_feature_unit_request(dev_addr, iface, fu_id,
					  VOLUME_CONTROL, channel,
					  AUDIO_REQ_SET_CUR,
					  &volume, sizeof(volume));
}

/*
 * Get the current volume of a feature unit.
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @fu_id:       Feature Unit entity ID
 * @channel:     Channel number (0 = master, 1..n = per-channel)
 * @out_volume:  Receives volume in 1/256 dB units
 *
 * Returns 0 on success, negative errno on failure.
 */
int audio_get_volume(uint8_t dev_addr, uint8_t iface,
		     uint8_t fu_id, uint8_t channel, int16_t *out_volume)
{
	int16_t val = 0;
	int ret;

	if (!out_volume)
		return -EINVAL;

	ret = audio_feature_unit_request(dev_addr, iface, fu_id,
					 VOLUME_CONTROL, channel,
					 AUDIO_REQ_GET_CUR,
					 &val, sizeof(val));
	if (ret < 0)
		return ret;

	*out_volume = val;
	return 0;
}

/*
 * Get the volume range (min, max, resolution) of a feature unit.
 * @dev_addr:    USB device address
 * @iface:       AudioControl interface number
 * @fu_id:       Feature Unit entity ID
 * @channel:     Channel number (0 = master)
 * @out_min:     Receives minimum volume in 1/256 dB
 * @out_max:     Receives maximum volume in 1/256 dB
 * @out_res:     Receives volume resolution (step) in 1/256 dB
 *
 * Returns 0 on success, negative errno on failure.
 */
int audio_get_volume_range(uint8_t dev_addr, uint8_t iface,
			   uint8_t fu_id, uint8_t channel,
			   int16_t *out_min, int16_t *out_max,
			   int16_t *out_res)
{
	int16_t val;
	int ret;

	if (!out_min || !out_max || !out_res)
		return -EINVAL;

	ret = audio_feature_unit_request(dev_addr, iface, fu_id,
					 VOLUME_CONTROL, channel,
					 AUDIO_REQ_GET_MIN,
					 &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_min = val;

	ret = audio_feature_unit_request(dev_addr, iface, fu_id,
					 VOLUME_CONTROL, channel,
					 AUDIO_REQ_GET_MAX,
					 &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_max = val;

	ret = audio_feature_unit_request(dev_addr, iface, fu_id,
					 VOLUME_CONTROL, channel,
					 AUDIO_REQ_GET_RES,
					 &val, sizeof(val));
	if (ret < 0)
		return ret;
	*out_res = val;

	return 0;
}

/* ── UAC1 descriptor parsing ─────────────────────────────────────────── */

/*
 * Parse a single audio control descriptor from the configuration data
 * and populate the audio device structure accordingly.
 *
 * @dev:         Audio device instance
 * @data:        Pointer to the raw descriptor
 * @length:      Total descriptor length (bLength)
 *
 * Returns 0 on success, negative errno on parse error.
 */
static int parse_audio_control_desc(struct audio_device *dev,
				    const uint8_t *data, uint8_t length)
{
	uint8_t subtype;

	if (!data || length < 3)
		return -EINVAL;

	subtype = data[2]; /* bDescriptorSubtype at offset 2 */

	switch (subtype) {
	case AC_INPUT_TERMINAL: {
		/* struct: bLength(1) bDescType(1) bDescSubtype(1)
		 *   bTerminalID(1) wTerminalType(2) bAssocTerminal(1)
		 *   bNrChannels(1) wChannelConfig(2) iChannelNames(1)
		 *   iTerminal(1)
		 * Total: 12 bytes (UAC1)
		 */
		if (length < 12) {
			kprintf("[usb_audio] short input terminal desc (%u)\n",
				length);
			return -EINVAL;
		}
		if (dev->num_terminals >= AUDIO_MAX_TERMINALS) {
			kprintf("[usb_audio] too many terminals\n");
			return -ENOSPC;
		}

		struct audio_terminal *t = &dev->terminals[dev->num_terminals];
		t->id = data[3];
		t->terminal_type = *(const uint16_t *)(data + 4);
		t->assoc_terminal = data[6];
		t->nr_channels = data[7];
		t->channel_config = *(const uint16_t *)(data + 8);
		t->is_input = 1;

		kprintf("[usb_audio] Input Terminal id=%u type=0x%04X "
			"channels=%u\n",
			t->id, t->terminal_type, t->nr_channels);

		dev->num_terminals++;
		return 0;
	}

	case AC_OUTPUT_TERMINAL: {
		/* struct: bLength(1) bDescType(1) bDescSubtype(1)
		 *   bTerminalID(1) wTerminalType(2) bAssocTerminal(1)
		 *   bSourceID(1) iTerminal(1)
		 * Total: 9 bytes (UAC1)
		 */
		if (length < 9) {
			kprintf("[usb_audio] short output terminal desc (%u)\n",
				length);
			return -EINVAL;
		}
		if (dev->num_terminals >= AUDIO_MAX_TERMINALS) {
			kprintf("[usb_audio] too many terminals\n");
			return -ENOSPC;
		}

		struct audio_terminal *t = &dev->terminals[dev->num_terminals];
		t->id = data[3];
		t->terminal_type = *(const uint16_t *)(data + 4);
		t->assoc_terminal = data[6];
		t->is_input = 0;

		/* bSourceID at offset 7 for output terminal */
		uint8_t src_id = data[7];
		(void)src_id; /* used for topology tracing */

		kprintf("[usb_audio] Output Terminal id=%u type=0x%04X "
			"source=%u\n",
			t->id, t->terminal_type, src_id);

		dev->num_terminals++;
		return 0;
	}

	case AC_FEATURE_UNIT: {
		/* struct: bLength(1) bDescType(1) bDescSubtype(1)
		 *   bUnitID(1) bSourceID(1) bControlSize(1)
		 *   bmaControls(0) [bControlSize bytes]
		 *   bmaControls(1..n) [bControlSize bytes each]
		 *   iFeature(1)
		 *
		 * Channel count = (bLength - 7 - bControlSize) / bControlSize
		 */
		if (length < 8) {  /* minimum: header + id + source + size + 1 control */
			kprintf("[usb_audio] short feature unit desc (%u)\n",
				length);
			return -EINVAL;
		}

		if (dev->num_feature_units >= AUDIO_MAX_FEATURE_UNITS) {
			kprintf("[usb_audio] too many feature units\n");
			return -ENOSPC;
		}

		struct audio_feature_unit *fu =
			&dev->feature_units[dev->num_feature_units];
		fu->id = data[3];
		fu->source_id = data[4];
		fu->control_size = data[5];

		if (fu->control_size == 0 || fu->control_size > 2) {
			kprintf("[usb_audio] unsupported control size %u\n",
				fu->control_size);
			return -EINVAL;
		}

		/* Parse controls: master (channel 0) + per-channel */
		uint8_t cs = fu->control_size;
		uint8_t offset = 6;  /* start of bmaControls[0] */
		int num_controls = 0;

		while (offset + cs + 1 <= length) {
			/* +1 for iFeature at end */
			uint16_t ctrl = 0;
			if (cs == 1) {
				ctrl = data[offset];
			} else {
				ctrl = *(const uint16_t *)(data + offset);
			}

			if (num_controls == 0) {
				fu->master_controls = ctrl;
			} else if (num_controls - 1 < AUDIO_MAX_CHANNELS) {
				fu->channel_controls[num_controls - 1] = ctrl;
			}

			offset += cs;
			num_controls++;
		}

		/* iFeature is the last byte */
		fu->feature_index = data[length - 1];

		/* Number of logical channels = num_controls - 1 (master) */
		fu->num_channels = (num_controls > 0) ? num_controls - 1 : 0;
		if (fu->num_channels > AUDIO_MAX_CHANNELS)
			fu->num_channels = AUDIO_MAX_CHANNELS;

		kprintf("[usb_audio] Feature Unit id=%u source=%u "
			"master_ctl=0x%04X channels=%u\n",
			fu->id, fu->source_id,
			fu->master_controls, fu->num_channels);

		dev->num_feature_units++;
		return 0;
	}

	case AC_HEADER:
	case AC_MIXER_UNIT:
	case AC_SELECTOR_UNIT:
	case AC_PROCESSING_UNIT:
	case AC_EXTENSION_UNIT:
		/* Acknowledge known subtypes but skip parsing for now */
		return 0;

	default:
		kprintf("[usb_audio] unknown AC descriptor subtype 0x%02X\n",
			subtype);
		return 0; /* skip unknown, not an error */
	}
}

/*
 * Parse an AudioStreaming interface descriptor.
 *
 * @dev:         Audio device instance
 * @data:        Pointer to the raw descriptor data
 * @length:      Total descriptor length
 *
 * Returns 0 on success, negative errno on error.
 */
static int parse_audio_streaming_desc(struct audio_device *dev,
				      const uint8_t *data, uint8_t length)
{
	uint8_t subtype;

	if (!data || length < 3)
		return -EINVAL;

	subtype = data[2];

	switch (subtype) {
	case AS_GENERAL: {
		/* struct: bLength(1) bDescType(1) bDescSubtype(1)
		 *   bTerminalLink(1) bDelay(1) wFormatTag(2)
		 * Total: 7 bytes
		 */
		if (length < 7)
			return -EINVAL;

		uint8_t term_link = data[3];
		uint16_t fmt_tag = *(const uint16_t *)(data + 5);

		kprintf("[usb_audio] AS General: terminal_link=%u "
			"fmt_tag=0x%04X\n", term_link, fmt_tag);
		return 0;
	}

	case AS_FORMAT_TYPE: {
		/* struct: bLength(1) bDescType(1) bDescSubtype(1)
		 *   bFormatType(1) bNrChannels(1) bSubframeSize(1)
		 *   bBitResolution(1) bSamFreqType(1)
		 *   tSamFreq[...] (3 bytes each, or 3 bytes for continuous)
		 *
		 * bSamFreqType=0 means continuous range (tLower+tUpper).
		 * bSamFreqType>0 means discrete frequencies.
		 * Total: variable
		 */
		if (length < 8)
			return -EINVAL;

		uint8_t fmt_type = data[3];
		if (fmt_type != FORMAT_TYPE_I) {
			/* Skip non-PCM format types */
			return 0;
		}

		dev->num_channels = data[4];
		uint8_t subframe_size = data[5];
		dev->bit_resolution = data[6];
		uint8_t sam_freq_type = data[7];

		kprintf("[usb_audio] Format Type I: channels=%u "
			"subframe=%u bits=%u freq_type=%u\n",
			dev->num_channels, subframe_size,
			dev->bit_resolution, sam_freq_type);

		if (sam_freq_type == 0) {
			/* Continuous frequency range: 3 bytes lower + 3 upper */
			if (length < 14)
				return -EINVAL;

			uint32_t freq_min = (uint32_t)data[8] |
					   ((uint32_t)data[9] << 8) |
					   ((uint32_t)data[10] << 16);
			uint32_t freq_max = (uint32_t)data[11] |
					   ((uint32_t)data[12] << 8) |
					   ((uint32_t)data[13] << 16);

			/* Add a representative frequency point */
			if (dev->num_sample_rates < AUDIO_MAX_SAMPLE_RATES) {
				dev->sample_rates[dev->num_sample_rates++] =
					freq_min;
			}
			if (dev->num_sample_rates < AUDIO_MAX_SAMPLE_RATES &&
			    freq_max > freq_min) {
				dev->sample_rates[dev->num_sample_rates++] =
					freq_max;
			}

			kprintf("[usb_audio]   continuous range: %u-%u Hz\n",
				freq_min, freq_max);
		} else {
			/* Discrete frequencies */
			uint8_t freq_offset = 8;
			uint8_t num_freqs = sam_freq_type;

			for (uint8_t i = 0; i < num_freqs; i++) {
				if (freq_offset + 3 > length)
					break;
				uint32_t freq = (uint32_t)data[freq_offset] |
						((uint32_t)data[freq_offset + 1] << 8) |
						((uint32_t)data[freq_offset + 2] << 16);
				freq_offset += 3;

				if (dev->num_sample_rates < AUDIO_MAX_SAMPLE_RATES)
					dev->sample_rates[dev->num_sample_rates++] = freq;
			}
		}
		return 0;
	}

	default:
		return 0;
	}
}

/*
 * Parse isochronous endpoint descriptors for an AudioStreaming interface.
 *
 * @dev:         Audio device instance
 * @data:        Raw endpoint descriptor data
 * @length:      Descriptor length
 *
 * Returns 0 on success, negative errno on error.
 */
static int parse_audio_streaming_ep(struct audio_device *dev,
				    const uint8_t *data, uint8_t length)
{
	const struct usb_endpoint_descriptor *ep;
	uint8_t ep_addr;
	uint8_t ep_attr;

	if (!data || length < sizeof(struct usb_endpoint_descriptor))
		return -EINVAL;

	ep = (const struct usb_endpoint_descriptor *)data;
	ep_addr = ep->bEndpointAddress;
	ep_attr = ep->bmAttributes;

	/* Only interested in isochronous endpoints */
	if ((ep_attr & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_ISOCH)
		return 0;

	uint16_t max_pkt = ep->wMaxPacketSize;
	uint8_t interval = ep->bInterval;

	if (ep_addr & USB_ENDPOINT_DIR_IN) {
		dev->iso_in_ep = ep_addr;
		kprintf("[usb_audio]   ISO IN ep 0x%02X max=%u interval=%u\n",
			ep_addr, max_pkt, interval);
	} else {
		dev->iso_out_ep = ep_addr;
		kprintf("[usb_audio]   ISO OUT ep 0x%02X max=%u interval=%u\n",
			ep_addr, max_pkt, interval);
	}

	if (max_pkt > dev->max_packet_size)
		dev->max_packet_size = max_pkt;

	return 0;
}

/* ── Topology discovery ───────────────────────────────────────────────── */

/*
 * Find a feature unit by its entity ID in the audio device topology.
 * Returns a pointer to the feature unit, or NULL if not found.
 */
static struct audio_feature_unit *find_feature_unit_by_id(
	struct audio_device *dev, uint8_t id)
{
	for (int i = 0; i < dev->num_feature_units; i++) {
		if (dev->feature_units[i].id == id)
			return &dev->feature_units[i];
	}
	return NULL;
}

/*
 * Find a terminal by its ID in the audio device topology.
 * Returns a pointer to the terminal, or NULL if not found.
 */
static struct audio_terminal *find_terminal_by_id(
	struct audio_device *dev, uint8_t id)
{
	for (int i = 0; i < dev->num_terminals; i++) {
		if (dev->terminals[i].id == id)
			return &dev->terminals[i];
	}
	return NULL;
}

/*
 * Print the audio function topology for debugging.
 */
static void dump_audio_topology(struct audio_device *dev)
{
	kprintf("[usb_audio] Audio topology for dev %u:\n", dev->dev_addr);

	for (int i = 0; i < dev->num_terminals; i++) {
		struct audio_terminal *t = &dev->terminals[i];
		kprintf("[usb_audio]   %s Terminal id=%u type=0x%04X\n",
			t->is_input ? "Input" : "Output",
			t->id, t->terminal_type);
	}

	for (int i = 0; i < dev->num_feature_units; i++) {
		struct audio_feature_unit *fu = &dev->feature_units[i];
		kprintf("[usb_audio]   Feature Unit id=%u source=%u "
			"mute=%s volume=%s\n",
			fu->id, fu->source_id,
			(fu->master_controls & FU_CTRL_MUTE) ? "yes" : "no",
			(fu->master_controls & FU_CTRL_VOLUME) ? "yes" : "no");
	}
}

/* ── Configuration descriptor walking ─────────────────────────────────── */

/*
 * Callback function for usb_for_each_config_subdesc.
 * Dispatches audio-specific descriptors to the appropriate parser.
 */
struct audio_parse_ctx {
	struct audio_device *dev;
	int    ac_iface_found;    /* audio control interface seen */
	int    as_iface_found;    /* audio streaming interface seen */
	uint8_t current_iface;    /* current interface number */
	uint8_t current_iface_class;
	uint8_t current_iface_subclass;
};

static int audio_desc_callback(uint8_t bDescriptorType,
			       const uint8_t *data, uint8_t bLength,
			       void *user_data)
{
	struct audio_parse_ctx *ctx = (struct audio_parse_ctx *)user_data;
	struct audio_device *dev = ctx->dev;
	int ret = 0;

	if (bDescriptorType == USB_DT_INTERFACE) {
		const struct usb_interface_descriptor *iface =
			(const struct usb_interface_descriptor *)data;
		ctx->current_iface = iface->bInterfaceNumber;
		ctx->current_iface_class = iface->bInterfaceClass;
		ctx->current_iface_subclass = iface->bInterfaceSubClass;

		/* Track audio control interface */
		if (iface->bInterfaceClass == USB_CLASS_AUDIO &&
		    iface->bInterfaceSubClass == AUDIO_SUBCLASS_CONTROL) {
			ctx->ac_iface_found = 1;
			dev->ac_iface = iface->bInterfaceNumber;
		}

		/* Track audio streaming interface */
		if (iface->bInterfaceClass == USB_CLASS_AUDIO &&
		    iface->bInterfaceSubClass == AUDIO_SUBCLASS_STREAMING) {
			if (!ctx->as_iface_found) {
				ctx->as_iface_found = 1;
				dev->as_iface = iface->bInterfaceNumber;
				dev->alt_setting = iface->bAlternateSetting;
			}
		}
		return 0;
	}

	/* Class-specific audio descriptors (CS_INTERFACE) */
	if (bDescriptorType == CS_INTERFACE) {
		/* Only parse within audio control interfaces */
		if (ctx->current_iface_class == USB_CLASS_AUDIO &&
		    ctx->current_iface_subclass == AUDIO_SUBCLASS_CONTROL) {
			ret = parse_audio_control_desc(dev, data, bLength);
		}

		/* Parse audio streaming descriptors */
		if (ctx->current_iface_class == USB_CLASS_AUDIO &&
		    ctx->current_iface_subclass == AUDIO_SUBCLASS_STREAMING) {
			ret = parse_audio_streaming_desc(dev, data, bLength);
		}
		return ret;
	}

	/* Endpoint descriptors within audio streaming interfaces */
	if (bDescriptorType == USB_DT_ENDPOINT &&
	    ctx->current_iface_class == USB_CLASS_AUDIO &&
	    ctx->current_iface_subclass == AUDIO_SUBCLASS_STREAMING) {
		ret = parse_audio_streaming_ep(dev, data, bLength);
		return ret;
	}

	return 0;
}

/*
 * Probe a USB audio device — parse descriptors, discover topology.
 *
 * @dev_desc:    USB device descriptor from the core
 *
 * Returns 0 on success (device is audio), negative errno on failure.
 */
static int usb_audio_probe(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return -EINVAL;

	/* Only match audio class devices */
	if (dev_desc->class_code != USB_CLASS_AUDIO &&
	    dev_desc->class_code != 0) {
		return -ENODEV;
	}

	/* Allocate an audio device slot */
	if (g_audio_count >= AUDIO_MAX_DEVICES) {
		kprintf("[usb_audio] too many audio devices\n");
		return -ENOSPC;
	}

	struct audio_device *dev = &g_audio_devs[g_audio_count];
	memset(dev, 0, sizeof(struct audio_device));
	dev->dev_addr = dev_desc->addr;

	/* We need to read the full configuration descriptor.
	 * For now, use the device-level class info to check USB_CLASS_AUDIO.
	 * A fuller implementation would call usb_control_msg with
	 * USB_REQ_GET_DESCRIPTOR to read the config, then walk sub-descriptors.
	 */

	/* For UAC1, the device class may be 0 (specified at interface level)
	 * or USB_CLASS_AUDIO (specified at device level).
	 * If the device class is 0, we check if any interface has audio class.
	 */

	/* Try to read the configuration descriptor to discover topology.
	 * We request the first configuration (index 0).
	 */
	uint8_t config_buf[256];
	int ret;

	memset(config_buf, 0, sizeof(config_buf));

	/* Read config descriptor header first to get wTotalLength */
	struct usb_setup_packet setup;
	memset(&setup, 0, sizeof(setup));
	ret = usb_control_msg(dev_desc->addr,
			      USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
			      USB_REQ_GET_DESCRIPTOR,
			      (USB_DT_CONFIG << 8) | 0,
			      0, 4, config_buf);
	if (ret < 0) {
		kprintf("[usb_audio] failed to read config header: %d\n", ret);
		/* Non-fatal — we can still register the device */
		dev->active = 1;
		g_audio_count++;
		return 0;
	}

	uint16_t total_len = *(const uint16_t *)(config_buf + 2);
	if (total_len > sizeof(config_buf)) {
		kprintf("[usb_audio] config too large (%u), truncating\n",
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
		kprintf("[usb_audio] failed to read full config: %d\n", ret);
		dev->active = 1;
		g_audio_count++;
		return 0;
	}

	/* Walk sub-descriptors to discover audio topology */
	struct audio_parse_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.dev = dev;

	ret = usb_for_each_config_subdesc(config_buf, total_len,
					  audio_desc_callback, &ctx);
	if (ret < 0 && ret != -ENOENT) {
		kprintf("[usb_audio] descriptor walk error: %d\n", ret);
		/* Still mark as active — partial info is better than nothing */
	}

	/* Check if we found any audio interfaces */
	if (!ctx.ac_iface_found && !ctx.as_iface_found) {
		kprintf("[usb_audio] no audio interfaces found for dev %u\n",
			dev_desc->addr);
		/* This can happen if class is at interface level and we
		 * got called for a non-audio device — return failure.
		 */
		return -ENODEV;
	}

	/* Dump discovered topology */
	dump_audio_topology(dev);

	/* If we found feature units with mute/volume control, cache initial values */
	for (int i = 0; i < dev->num_feature_units; i++) {
		struct audio_feature_unit *fu = &dev->feature_units[i];

		if (fu->master_controls & FU_CTRL_MUTE) {
			uint8_t muted = 0;
			int r = audio_get_mute(dev->dev_addr, dev->ac_iface,
					       fu->id, &muted);
			if (r == 0) {
				dev->mute_state = muted;
				kprintf("[usb_audio] FU id=%u initial mute=%u\n",
					fu->id, muted);
			}
		}

		if (fu->master_controls & FU_CTRL_VOLUME) {
			int16_t vol = 0;
			int r = audio_get_volume(dev->dev_addr, dev->ac_iface,
						 fu->id, 0, &vol);
			if (r == 0) {
				dev->volume = vol;
				kprintf("[usb_audio] FU id=%u initial volume=%d\n",
					fu->id, vol);
			}
		}
	}

	dev->active = 1;
	g_audio_count++;

	kprintf("[usb_audio] audio device %u probed: %d terminals, "
		"%d feature units\n",
		dev_desc->addr, dev->num_terminals, dev->num_feature_units);

	return 0;
}

/* ── Disconnect ───────────────────────────────────────────────────────── */

static void usb_audio_disconnect(const struct usb_device *dev_desc)
{
	if (!dev_desc)
		return;

	/* Find and deactivate the audio device */
	for (int i = 0; i < g_audio_count; i++) {
		if (g_audio_devs[i].dev_addr == dev_desc->addr &&
		    g_audio_devs[i].active) {
			g_audio_devs[i].active = 0;
			kprintf("[usb_audio] device %u disconnected\n",
				dev_desc->addr);
			break;
		}
	}
}

/* ── USB device ID table ─────────────────────────────────────────────── */

static const struct usb_device_id usb_audio_ids[] = {
	/* Match all USB Audio Class 1.0 devices by class */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS |
			 USB_DEVICE_ID_MATCH_DEV_SUBCLASS |
			 USB_DEVICE_ID_MATCH_DEV_PROTOCOL,
	  .class = USB_CLASS_AUDIO, .subclass = 0x00, .protocol = 0x00 },
	/* Also match devices that specify audio class at interface level
	 * (device class = 0, subclass = 0, protocol = 0 — interface decides).
	 * These will be probed and rejected by the probe function if no
	 * audio interfaces are found.
	 */
	{ .match_flags = USB_DEVICE_ID_MATCH_DEV_CLASS,
	  .class = 0x00, .subclass = 0x00, .protocol = 0x00 },
	USB_DEVICE_TABLE_END
};

/* ── Driver registration ─────────────────────────────────────────────── */

static struct usb_driver g_usb_audio_driver = {
	.name       = "usb_audio",
	.id_table   = usb_audio_ids,
	.probe      = usb_audio_probe,
	.disconnect = usb_audio_disconnect,
};

void __init usb_audio_init(void)
{
	if (g_initialized)
		return;

	memset(g_audio_devs, 0, sizeof(g_audio_devs));
	g_audio_count = 0;
	g_initialized = 1;

	usb_register_driver(&g_usb_audio_driver);

	kprintf("[usb_audio] USB Audio Class 1.0 driver registered\n");
}

void usb_audio_exit(void)
{
	usb_deregister_driver(&g_usb_audio_driver);

	/* Deactivate all devices */
	for (int i = 0; i < g_audio_count; i++)
		g_audio_devs[i].active = 0;

	g_audio_count = 0;
	g_initialized = 0;

	kprintf("[usb_audio] USB Audio Class 1.0 driver unregistered\n");
}

module_init(usb_audio_init);
module_exit(usb_audio_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("USB Audio Class 1.0 (UAC1) driver with feature unit controls");
MODULE_AUTHOR("OS Kernel Team");
MODULE_ALIAS("usb:v* p* d* dc01 dsc* dp*");          /* USB Audio Class */
