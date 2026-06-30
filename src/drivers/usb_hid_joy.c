/*
 * usb_hid_joy.c — USB HID joystick/gamepad driver (enhanced)
 *
 * Full gamepad driver with per-device event queue, hat switch (POV)
 * support, deadzone configuration, force feedback / rumble output,
 * and event-driven polling.  Supports up to GAMEPAD_MAX_DEVICES
 * simultaneous gamepads/joysticks.
 *
 * Each device maintains:
 *   - Axis array with per-axis deadzone, fuzz, and flat
 *   - Button bitfield (up to 128 buttons)
 *   - Hat switch state (up to 4 hats, 8-directional)
 *   - Event ring buffer for press/release/axis/hat changes
 *   - Rumble motor output support via HID Output reports
 *
 * References:
 *   USB HID Usage Tables, §4  — Generic Desktop Page (0x01)
 *   USB HID Usage Tables, §15 — Simulation Page (0x02)
 *   USB Device Class Definition for HID, Version 1.11
 *   Linux Documentation/input/joystick-api.txt
 *
 * Copyright (c) 2026 Rusik69 OS Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "uhid.h"
#include "usb.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct hid_gamepad_dev g_gamepads[GAMEPAD_MAX_DEVICES];
static int g_joy_count = 0;
static int g_joy_initialized = 0;

/* Per-device optional callbacks */
static void (*g_callbacks[GAMEPAD_MAX_DEVICES])(struct hid_gamepad_event *ev);

/* ── Hat switch direction mapping ──────────────────────────────────── */

/*
 * Convert raw hat switch value to direction code.
 * Most HID gamepads report hat as 0=up, 1=up-right, 2=right, ..., 7=up-left.
 * -1 or 8 = centered.
 * Returns 0=centered, 1=up, 2=right, 3=down, 4=left,
 *         5=up-right, 6=down-right, 7=down-left, 8=up-left.
 */
static int hat_value_to_direction(int32_t raw)
{
	switch (raw) {
	case 0:  return 1;  /* up */
	case 1:  return 5;  /* up-right */
	case 2:  return 2;  /* right */
	case 3:  return 6;  /* down-right */
	case 4:  return 3;  /* down */
	case 5:  return 7;  /* down-left */
	case 6:  return 4;  /* left */
	case 7:  return 8;  /* up-left */
	default: return 0;  /* centred */
	}
}

/* ── Event queue helpers ───────────────────────────────────────────── */

static void gamepad_queue_event(struct hid_gamepad_dev *dev,
                                 int type, int code, int32_t value)
{
	spinlock_acquire(&dev->lock);

	int next = (dev->ev_tail + 1) % GAMEPAD_EVENT_QUEUE;
	if (next != dev->ev_head) {
		dev->events[dev->ev_tail].type  = type;
		dev->events[dev->ev_tail].code  = code;
		dev->events[dev->ev_tail].value = value;
		dev->ev_tail = next;
	}

	spinlock_release(&dev->lock);
}

/* ── Report descriptor parser (gamepad-specific) ───────────────────── */

/*
 * Parse an HID report descriptor looking for gamepad/joystick collections.
 *
 * Scans for:
 *   - Collection(Application, Joystick)     — usage 0x04
 *   - Collection(Application, Gamepad)      — usage 0x05
 *   - Collection(Application, Multi-axis)   — usage 0x08
 *
 * Within those collections, counts:
 *   - Generic Desktop page usages (0x30–0x39) → axes with logical range
 *   - Button page usages                      → button count
 *   - Hat Switch usage (0x39)                 → hat switch count
 *
 * Returns 1 if gamepad/joystick found, 0 otherwise.
 * Writes expected report length in bytes to @out_report_len.
 * Writes flags showing what was found to @out_flags.
 */
static int gamepad_parse_report_desc(const uint8_t *desc, int len,
                                      int *out_report_len,
                                      struct hid_gamepad_dev *dev)
{
	if (!desc || len <= 0 || !out_report_len || !dev)
		return 0;

	int i = 0;
	uint32_t usage_page = 0;
	uint32_t cur_usage = 0;
	uint32_t cur_usage_min = 0;
	uint32_t cur_usage_max = 0;
	uint32_t cur_report_size = 8;
	uint32_t cur_report_count = 0;
	uint32_t logical_min = 0;
	uint32_t logical_max = 255;
	int in_gamepad_collection = 0;
	int collection_depth = 0;
	int total_bits = 0;
	int found_gamepad = 0;
	int axis_count = 0;
	int btn_count = 0;
	int hat_count = 0;

	/* Reset device counts */
	dev->n_axes = 0;
	dev->n_buttons = 0;
	dev->n_hats = 0;
	dev->is_gamepad = 0;

	while (i < len) {
		uint8_t prefix = desc[i];
		if (prefix == 0) {
			i++;
			continue;
		}

		uint8_t bTag  = (prefix >> 4) & 0x0F;
		uint8_t bType = (prefix >> 2) & 0x03;
		uint8_t bSize = prefix & 0x03;

		int data_bytes = (bSize == 3) ? 4 : (int)bSize;
		if (i + 1 + data_bytes > len)
			break;

		i++; /* consume prefix */

		uint32_t data = 0;
		if (data_bytes >= 1) data  = desc[i];
		if (data_bytes >= 2) data |= (uint32_t)desc[i + 1] << 8;
		if (data_bytes >= 4) {
			data |= (uint32_t)desc[i + 2] << 16;
			data |= (uint32_t)desc[i + 3] << 24;
		}

		switch (bType) {
		case 0: /* Main items */
			switch (bTag) {
			case 8: { /* Input */
				uint32_t flags = data;
				int is_const = (flags & HID_IOF_CONST) ? 1 : 0;

				if (in_gamepad_collection &&
				    cur_report_count > 0 &&
				    cur_report_size > 0 && !is_const) {
					total_bits += (int)(cur_report_count *
					                     cur_report_size);

					/* Check usage and count axes/buttons/hats */
					if (usage_page == HID_PAGE_GENERIC_DESKTOP) {
						/* Count axes (usages 0x30–0x39) */
						if (cur_usage >= 0x30 &&
						    cur_usage <= 0x39 &&
						    cur_usage != HID_USAGE_HAT_SWITCH) {
							int n = (cur_report_count > 0)
							        ? (int)cur_report_count : 1;
							axis_count += n;
						}
						/* Count hat switches (usage 0x39) */
						if (cur_usage == HID_USAGE_HAT_SWITCH ||
						    (cur_usage_min <= HID_USAGE_HAT_SWITCH &&
						     cur_usage_max >= HID_USAGE_HAT_SWITCH)) {
							hat_count++;
						}
						/* If usage range includes 0x30-0x39 */
						if (cur_usage_min >= 0x30 &&
						    cur_usage_min <= 0x39 &&
						    cur_usage_max >= 0x30 &&
						    cur_usage_max <= 0x39) {
							uint32_t range_start = cur_usage_min;
							uint32_t range_end = cur_usage_max;
							for (uint32_t u = range_start;
							     u <= range_end; u++) {
								if (u != HID_USAGE_HAT_SWITCH)
									axis_count++;
								else
									hat_count++;
							}
						}
					} else if (usage_page == HID_PAGE_BUTTONS) {
						/* Count buttons from range */
						if (cur_usage_max >= cur_usage_min &&
						    cur_usage_min > 0) {
							int n = (int)(cur_usage_max -
							              cur_usage_min + 1);
							btn_count += n;
						} else if (cur_usage > 0) {
							btn_count += (int)cur_report_count;
						}
					}

					found_gamepad = 1;
				}
				/* Reset local items after each Main item */
				cur_usage = 0;
				cur_usage_min = 0;
				cur_usage_max = 0;
				cur_report_count = 0;
				break;
			}

			case 9:  /* Output */
			case 11: /* Feature */
				cur_usage = 0;
				cur_usage_min = 0;
				cur_usage_max = 0;
				cur_report_count = 0;
				break;

			case 10: { /* Collection */
				uint8_t coll_type = (uint8_t)(data & 0xFF);
				collection_depth++;

				if (coll_type == HID_COLLECTION_APPLICATION &&
				    usage_page == HID_PAGE_GENERIC_DESKTOP) {
					if (cur_usage == HID_USAGE_JOYSTICK ||
					    cur_usage == HID_USAGE_GAMEPAD ||
					    cur_usage == HID_USAGE_MULTI_AXIS) {
						in_gamepad_collection = 1;
						if (cur_usage == HID_USAGE_GAMEPAD)
							dev->is_gamepad = 1;
						kprintf("[JOY] Found %s collection\n",
						        cur_usage == HID_USAGE_GAMEPAD
						        ? "Gamepad"
						        : cur_usage == HID_USAGE_JOYSTICK
						          ? "Joystick" : "Multi-axis");
					}
					/* Also check Simulation page collections */
				} else if (coll_type == HID_COLLECTION_APPLICATION &&
				           usage_page == HID_PAGE_SIMULATION) {
					/* Flight/auto sim devices are joystick-like */
					in_gamepad_collection = 1;
					dev->is_gamepad = 0;
					kprintf("[JOY] Found Simulation collection\n");
				}
				break;
			}

			case 12: /* End Collection */
				if (collection_depth > 0) {
					collection_depth--;
					if (collection_depth == 0)
						in_gamepad_collection = 0;
				}
				break;
			}
			break;

		case 1: /* Global items */
			switch (bTag) {
			case 0:  usage_page = data; break;
			case 1:  logical_min = data; break;
			case 2:  logical_max = data; break;
			case 7:  cur_report_size = data; break;
			case 8:  /* Report ID */ break;
			case 9:  cur_report_count = data; break;
			default: break;
			}
			break;

		case 2: /* Local items */
			switch (bTag) {
			case 0: cur_usage = data; break;
			case 1: cur_usage_min = data; break;
			case 2: cur_usage_max = data; break;
			default: break;
			}
			break;
		}

		i += data_bytes;
	}

	/* Commit counts to device */
	dev->n_axes = (axis_count > 0 && axis_count <= GAMEPAD_MAX_AXES)
	              ? axis_count : 0;
	dev->n_buttons = (btn_count > 0 && btn_count <= GAMEPAD_MAX_BUTTONS)
	                 ? btn_count : 0;
	dev->n_hats = (hat_count > 0 && hat_count <= 4) ? hat_count : 0;

	/* If no collections found by name, try heuristic detection */
	if (!found_gamepad && (axis_count > 0 || btn_count > 0)) {
		found_gamepad = 1;
	}

	/* Set default axis bounds */
	for (int a = 0; a < dev->n_axes; a++) {
		dev->axes[a].min = (int32_t)logical_min;
		dev->axes[a].max = (int32_t)logical_max;
		dev->axes[a].value = (int32_t)(((int32_t)logical_min +
		                                 (int32_t)logical_max) / 2);
		dev->axes[a].deadzone = 0;
		dev->axes[a].fuzz = 0;
		dev->axes[a].flat = 0;
	}

	/* Default values if descriptor parsing gave nothing */
	if (dev->n_axes == 0) {
		/* Assume 4 axes (X, Y, Rx, Ry) for unknown devices */
		dev->n_axes = 4;
		for (int a = 0; a < dev->n_axes; a++) {
			dev->axes[a].min = 0;
			dev->axes[a].max = 255;
			dev->axes[a].value = 127;
		}
	}
	if (dev->n_buttons == 0) {
		dev->n_buttons = 8;   /* at least 8 buttons */
	}

	if (found_gamepad && total_bits > 0) {
		*out_report_len = (total_bits + 7) / 8;
		kprintf("[JOY] Report length: %d bytes, %d axes, %d buttons, "
		        "%d hats\n", *out_report_len, dev->n_axes,
		        dev->n_buttons, dev->n_hats);
		return 1;
	}

	/* Even without Input items, if we saw a gamepad collection, default */
	if (dev->n_axes > 0 || dev->n_buttons > 0) {
		*out_report_len = 8;
		return 1;
	}

	return 0;
}

/* ── Input report parser ───────────────────────────────────────────── */

/*
 * Extract gamepad state from a raw HID input report.
 *
 * Walks the report descriptor to find Input items within gamepad
 * collections, then extracts axes, buttons, and hats from the report
 * data and generates events for any changes.
 */
static void gamepad_process_report(struct hid_gamepad_dev *dev,
                                    const uint8_t *report, int len)
{
	if (!dev || !report || len <= 0)
		return;
	if (!dev->report_desc || dev->report_desc_len <= 0)
		return;

	const uint8_t *desc = dev->report_desc;
	int dlen = dev->report_desc_len;

	int i = 0;
	uint32_t usage_page = 0;
	uint32_t cur_usage = 0;
	uint32_t cur_usage_min = 0;
	uint32_t cur_usage_max = 0;
	uint32_t cur_report_size = 8;
	uint32_t cur_report_count = 0;
	int in_gamepad = 0;
	int collection_depth = 0;
	int report_bit_offset = 0;

	while (i < dlen) {
		uint8_t prefix = desc[i];
		if (prefix == 0) { i++; continue; }

		uint8_t bTag  = (prefix >> 4) & 0x0F;
		uint8_t bType = (prefix >> 2) & 0x03;
		uint8_t bSize = prefix & 0x03;

		int data_bytes = (bSize == 3) ? 4 : (int)bSize;
		if (i + 1 + data_bytes > dlen)
			break;
		i++;

		uint32_t data = 0;
		if (data_bytes >= 1) data  = desc[i];
		if (data_bytes >= 2) data |= (uint32_t)desc[i + 1] << 8;
		if (data_bytes >= 4) {
			data |= (uint32_t)desc[i + 2] << 16;
			data |= (uint32_t)desc[i + 3] << 24;
		}

		switch (bType) {
		case 0:
			switch (bTag) {
			case 8: { /* Input */
				if (!in_gamepad || cur_report_count == 0)
					break;

				uint32_t flags = data;
				int is_const = (flags & HID_IOF_CONST) ? 1 : 0;
				int is_var   = (flags & HID_IOF_VARIABLE) ? 1 : 0;
				int bit_size = (int)cur_report_size;
				int count    = (int)cur_report_count;

				if (is_const) {
					report_bit_offset += bit_size * count;
					break;
				}

				if (usage_page == HID_PAGE_GENERIC_DESKTOP &&
				    is_var && bit_size <= 16) {
					/* Axis or hat switch */
					for (int b = 0; b < count; b++) {
						int byte_off = report_bit_offset / 8;
						int bit_off  = report_bit_offset % 8;
						int32_t raw_val = 0;

						if (bit_size <= 8 && byte_off < len) {
							raw_val = (int32_t)report[byte_off];
						} else if (bit_size <= 16 &&
						           byte_off + 1 < len) {
							raw_val = (int32_t)(report[byte_off] |
							    ((uint32_t)report[byte_off + 1] << 8));
						}

						uint32_t u = cur_usage;
						if (cur_usage_max >= cur_usage_min &&
						    (uint32_t)b < cur_usage_max -
						                  cur_usage_min + 1) {
							u = cur_usage_min + (uint32_t)b;
						}

						if (u >= 0x30 && u <= 0x39 &&
						    u != HID_USAGE_HAT_SWITCH) {
							/* Axis */
							int axis_idx = (int)(u - 0x30);
							if (axis_idx >= 0 &&
							    axis_idx < GAMEPAD_MAX_AXES &&
							    axis_idx < dev->n_axes) {
								int32_t centre =
								    (dev->axes[axis_idx].max +
								     dev->axes[axis_idx].min) / 2;
								int32_t dz =
								    dev->axes[axis_idx].deadzone;
								/* Apply deadzone */
								if (dz > 0) {
									int32_t diff = raw_val - centre;
									if (diff < 0) diff = -diff;
									if (diff <= dz)
										raw_val = centre;
								}

								if (raw_val !=
								    dev->axes[axis_idx].value) {
									dev->axes[axis_idx].value =
									    raw_val;
									gamepad_queue_event(dev,
									    GAMEPAD_EV_ABS,
									    axis_idx, raw_val);
								}
							}
						} else if (u == HID_USAGE_HAT_SWITCH) {
							/* Hat switch */
							int hat_idx = (dev->n_hats > 0)
							              ? dev->n_hats - 1 : 0;
							if (hat_idx < 4) {
								int dir = hat_value_to_direction(
								    raw_val);
								if (dir != dev->hats[hat_idx]) {
									dev->hats[hat_idx] = dir;
									gamepad_queue_event(dev,
									    GAMEPAD_EV_HAT,
									    hat_idx, dir);
								}
							}
						}

						report_bit_offset += bit_size;
					}
				} else if (usage_page == HID_PAGE_BUTTONS) {
					/* Buttons */
					uint32_t base_usage = (cur_usage_min > 0)
					                       ? cur_usage_min
					                       : cur_usage;
					if (base_usage == 0)
						base_usage = 1;

					for (int b = 0; b < count; b++) {
						int byte_off = report_bit_offset / 8;
						int bit_off  = report_bit_offset % 8;
						int btn_idx  = (int)(base_usage + b - 1);

						int bit_val = 0;
						if (bit_size == 1 && byte_off < len) {
							bit_val = (report[byte_off] >>
							           bit_off) & 1;
						} else if (bit_size <= 16 &&
						           byte_off < len) {
							/* Array-style: value is button
							 * index, 0 = no press */
							int32_t v = 0;
							if (bit_size <= 8) {
								v = (int32_t)report[byte_off];
							} else if (byte_off + 1 < len) {
								v = (int32_t)(report[byte_off] |
								    ((uint32_t)report[byte_off+1]
								     << 8));
							}
							bit_val = (v == (int32_t)(base_usage + b))
							          ? 1 : 0;
						}

						if (btn_idx >= 0 &&
						    btn_idx < GAMEPAD_MAX_BUTTONS &&
						    btn_idx < dev->n_buttons) {
							uint8_t old = dev->buttons[btn_idx];
							dev->buttons[btn_idx] =
							    (uint8_t)bit_val;
							if ((uint8_t)bit_val != old) {
								gamepad_queue_event(dev,
								    GAMEPAD_EV_KEY,
								    btn_idx, bit_val);
							}
						}

						report_bit_offset += bit_size;
					}
				} else if (bit_size > 0) {
					report_bit_offset += bit_size * count;
				}

				/* Reset local items after Main item */
				cur_usage = 0;
				cur_usage_min = 0;
				cur_usage_max = 0;
				cur_report_count = 0;
				break;
			}

			case 10: { /* Collection */
				uint8_t coll_type = (uint8_t)(data & 0xFF);
				collection_depth++;
				if (coll_type == HID_COLLECTION_APPLICATION &&
				    usage_page == HID_PAGE_GENERIC_DESKTOP) {
					if (cur_usage == HID_USAGE_JOYSTICK ||
					    cur_usage == HID_USAGE_GAMEPAD ||
					    cur_usage == HID_USAGE_MULTI_AXIS)
						in_gamepad = 1;
				} else if (coll_type ==
				           HID_COLLECTION_APPLICATION &&
				           usage_page == HID_PAGE_SIMULATION) {
					in_gamepad = 1;
				}
				break;
			}

			case 12: /* End Collection */
				if (collection_depth > 0) {
					collection_depth--;
					if (collection_depth == 0)
						in_gamepad = 0;
				}
				break;
			}

			cur_usage = 0;
			cur_usage_min = 0;
			cur_usage_max = 0;
			cur_report_count = 0;
			break;

		case 1: /* Global items */
			switch (bTag) {
			case 0: usage_page = data; break;
			case 7: cur_report_size = data; break;
			case 8: break; /* Report ID */
			case 9: cur_report_count = data; break;
			default: break;
			}
			break;

		case 2: /* Local items */
			switch (bTag) {
			case 0: cur_usage = data; break;
			case 1: cur_usage_min = data; break;
			case 2: cur_usage_max = data; break;
			default: break;
			}
			break;
		}

		i += data_bytes;
	}
}

/* ── Rumble / force feedback output ────────────────────────────────── */

/*
 * Send a rumble (force feedback) output report to the device.
 *
 * Most gamepads with force feedback implement a simple Output report
 * with two bytes: strong motor (low-frequency) and weak motor
 * (high-frequency), often at Report ID 1.
 *
 * This sends via a HID SET_REPORT(Output) control transfer.
 */
int usb_hid_joy_set_rumble(int joy_idx, uint8_t strong, uint8_t weak)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return -ENODEV;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present)
		return -ENODEV;

	dev->rumble_strong = strong;
	dev->rumble_weak   = weak;

	/* Build a minimal output report: Report ID 1 (if present),
	 * then strong, then weak motor values.  Try without report
	 * ID first. */
	uint8_t report[4] = { 0 };
	report[0] = strong;
	report[1] = weak;

	int ret = usb_hid_set_report(HID_REPORT_OUTPUT, 0,
	                             report, 2);

	if (ret < 0) {
		/* Try with Report ID = 1 */
		report[0] = 1;
		report[1] = strong;
		report[2] = weak;
		ret = usb_hid_set_report(HID_REPORT_OUTPUT, 1,
		                         report + 1, 2);
	}

	if (ret == 0) {
		kprintf("[JOY] Rumble set: strong=%u weak=%u (dev %d)\n",
		        (unsigned)strong, (unsigned)weak, joy_idx);
	}

	return ret;
}

/* ── Deadzone configuration ─────────────────────────────────────────── */

int usb_hid_joy_set_deadzone(int joy_idx, int axis, int32_t deadzone)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return -ENODEV;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present)
		return -ENODEV;

	if (axis == -1) {
		/* Set deadzone for all axes */
		for (int a = 0; a < dev->n_axes; a++) {
			dev->axes[a].deadzone = deadzone;
		}
		return 0;
	}

	if (axis < 0 || axis >= dev->n_axes)
		return -EINVAL;

	dev->axes[axis].deadzone = deadzone;
	return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int usb_hid_joy_init(void)
{
	if (g_joy_initialized)
		return 0;

	memset(g_gamepads, 0, sizeof(g_gamepads));
	memset(g_callbacks, 0, sizeof(g_callbacks));
	g_joy_count = 0;
	g_joy_initialized = 1;

	kprintf("[JOY] Gamepad/joystick subsystem initialised\n");
	return 0;
}

int usb_hid_joy_register(uint8_t dev_addr, uint16_t vid, uint16_t pid,
                          uint8_t intf_num, uint8_t input_ep,
                          uint8_t interval,
                          const uint8_t *report_desc, int desc_len)
{
	if (!g_joy_initialized)
		return -EAGAIN;

	if (g_joy_count >= GAMEPAD_MAX_DEVICES)
		return -ENOSPC;

	struct hid_gamepad_dev *dev = &g_gamepads[g_joy_count];
	memset(dev, 0, sizeof(*dev));

	dev->dev_addr  = dev_addr;
	dev->vendor_id = vid;
	dev->product_id = pid;
	dev->intf_num  = intf_num;
	dev->input_ep  = input_ep;
	dev->input_ep_interval = interval;
	dev->ev_head   = 0;
	dev->ev_tail   = 0;
	spinlock_init(&dev->lock);

	/* Parse the report descriptor */
	int report_len = 8; /* default */
	int found = 0;
	if (report_desc && desc_len > 0) {
		found = gamepad_parse_report_desc(report_desc, desc_len,
		                                   &report_len, dev);

		/* Store a copy of the report descriptor */
		dev->report_desc = (uint8_t *)kmalloc((size_t)desc_len);
		if (dev->report_desc) {
			memcpy(dev->report_desc, report_desc, (size_t)desc_len);
			dev->report_desc_len = desc_len;
		}
	}

	if (!found) {
		/* Default reasonable values */
		dev->n_axes    = 4;
		dev->n_buttons = 8;
		dev->n_hats    = 1;
		for (int a = 0; a < dev->n_axes; a++) {
			dev->axes[a].min = 0;
			dev->axes[a].max = 255;
			dev->axes[a].value = 127;
		}
		kprintf("[JOY] No gamepad collection found, using defaults\n");
	}

	dev->report_len = report_len > 0 ? report_len : 8;
	dev->present = 1;

	int idx = g_joy_count;
	g_joy_count++;

	kprintf("[JOY] USB %s registered: VID=0x%04x PID=0x%04x "
	        "%d axes %d buttons %d hats (addr=%d, ep=0x%02x)\n",
	        dev->is_gamepad ? "gamepad" : "joystick",
	        vid, pid, dev->n_axes, dev->n_buttons, dev->n_hats,
	        dev_addr, input_ep);

	return idx;
}

void usb_hid_joy_unregister(int joy_idx)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present)
		return;

	dev->present = 0;

	if (dev->report_desc) {
		kfree(dev->report_desc);
		dev->report_desc = NULL;
	}
	dev->report_desc_len = 0;

	g_callbacks[joy_idx] = NULL;

	kprintf("[JOY] USB gamepad/joystick %d unregistered\n", joy_idx);
}

void usb_hid_joy_input(int joy_idx, const uint8_t *report, int len)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present || !report || len <= 0)
		return;

	if (len >= dev->report_len) {
		gamepad_process_report(dev, report, len);
	}
}

/* ── State query ────────────────────────────────────────────────────── */

int usb_hid_joy_get_axis(int joy_idx, int axis)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present || axis < 0 || axis >= dev->n_axes)
		return 0;

	return dev->axes[axis].value;
}

int usb_hid_joy_get_button(int joy_idx, int btn)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present || btn < 0 || btn >= dev->n_buttons)
		return 0;

	return dev->buttons[btn] ? 1 : 0;
}

int usb_hid_joy_get_axis_count(int joy_idx)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;
	return g_gamepads[joy_idx].n_axes;
}

int usb_hid_joy_get_button_count(int joy_idx)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;
	return g_gamepads[joy_idx].n_buttons;
}

int usb_hid_joy_get_hat_count(int joy_idx)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;
	return g_gamepads[joy_idx].n_hats;
}

int usb_hid_joy_get_hat(int joy_idx, int hat_idx)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return 0;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present || hat_idx < 0 || hat_idx >= dev->n_hats)
		return 0;

	return dev->hats[hat_idx];
}

int usb_hid_joy_get_count(void)
{
	return g_joy_count;
}

int usb_hid_joy_get_event(int joy_idx, struct hid_gamepad_event *out)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return -ENODEV;
	if (!out)
		return -EINVAL;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];

	spinlock_acquire(&dev->lock);

	if (dev->ev_head == dev->ev_tail) {
		spinlock_release(&dev->lock);
		return 0;
	}

	*out = dev->events[dev->ev_head];
	dev->ev_head = (dev->ev_head + 1) % GAMEPAD_EVENT_QUEUE;

	spinlock_release(&dev->lock);
	return 1;
}

/* ── Polling ────────────────────────────────────────────────────────── */

void usb_hid_joy_poll(int joy_idx)
{
	struct hid_gamepad_dev *dev;
	uint8_t buf[64];
	int ret;
	int report_len;

	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return;

	dev = &g_gamepads[joy_idx];
	if (!dev->present || !dev->input_ep || !dev->report_len)
		return;

	report_len = dev->report_len;
	if (report_len > (int)sizeof(buf))
		report_len = (int)sizeof(buf);

	/* Read the interrupt IN endpoint */
	ret = usb_hid_get_report(HID_REPORT_INPUT, 0, buf, (size_t)report_len);
	if (ret == 0) {
		gamepad_process_report(dev, buf, report_len);
	}
}

void usb_hid_joy_poll_all(void)
{
	for (int i = 0; i < g_joy_count; i++) {
		usb_hid_joy_poll(i);
	}
}

/* ── Callback management ────────────────────────────────────────────── */

void usb_hid_joy_set_callback(int joy_idx,
                               void (*cb)(struct hid_gamepad_event *ev))
{
	if (joy_idx >= 0 && joy_idx < GAMEPAD_MAX_DEVICES) {
		g_callbacks[joy_idx] = cb;
	}
}

/* ── Userspace read (for /dev/input/js* access) ─────────────────────── */

int usb_hid_joy_read(int joy_idx, void *buf, size_t count)
{
	struct hid_gamepad_event ev;
	int written = 0;
	uint8_t *out = (uint8_t *)buf;

	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return -ENODEV;
	if (!buf || count == 0)
		return -EINVAL;

	while (count >= (size_t)sizeof(struct hid_gamepad_event)) {
		int ret = usb_hid_joy_get_event(joy_idx, &ev);
		if (ret <= 0)
			break;

		memcpy(out, &ev, sizeof(ev));
		out     += sizeof(ev);
		written += sizeof(ev);
		count   -= sizeof(ev);
	}

	if (written == 0)
		return -EAGAIN;

	return written;
}

/* ── Ioctl ──────────────────────────────────────────────────────────── */

int usb_hid_joy_ioctl(int joy_idx, int cmd, void *arg)
{
	if (joy_idx < 0 || joy_idx >= g_joy_count)
		return -ENODEV;

	struct hid_gamepad_dev *dev = &g_gamepads[joy_idx];
	if (!dev->present)
		return -ENODEV;

	switch (cmd) {
	case JOYIOC_GAXES: {
		int n = dev->n_axes;
		if (arg) {
			int *p = (int *)arg;
			*p = n;
		}
		return n;
	}

	case JOYIOC_GBUTTONS: {
		int n = dev->n_buttons;
		if (arg) {
			int *p = (int *)arg;
			*p = n;
		}
		return n;
	}

	case JOYIOC_GHATS: {
		int n = dev->n_hats;
		if (arg) {
			int *p = (int *)arg;
			*p = n;
		}
		return n;
	}

	case JOYIOC_GDEADZONE: {
		/* arg points to int axis number, return deadzone value */
		if (!arg)
			return -EINVAL;
		int axis = *(int *)arg;
		if (axis < 0 || axis >= dev->n_axes)
			return -EINVAL;
		return dev->axes[axis].deadzone;
	}

	case JOYIOC_SDEADZONE: {
		/* arg points to int deadzone value */
		if (!arg)
			return -EINVAL;
		int32_t dz = *(int32_t *)arg;
		/* Set for all axes */
		for (int a = 0; a < dev->n_axes; a++) {
			dev->axes[a].deadzone = dz;
		}
		return 0;
	}

	case JOYIOC_GRUMBLE: {
		/* Return 1 if rumble capability likely present */
		return 1;
	}

	case JOYIOC_SRUMBLE: {
		if (!arg)
			return -EINVAL;
		struct joy_rumble *r = (struct joy_rumble *)arg;
		return usb_hid_joy_set_rumble(joy_idx, r->strong, r->weak);
	}

	case JOYIOC_GAME_PAD: {
		return dev->is_gamepad ? 1 : 0;
	}

	case JOYIOC_CALIBRATE: {
		/* Re-centre all axes */
		for (int a = 0; a < dev->n_axes; a++) {
			dev->axes[a].deadzone = 0;
			dev->axes[a].value =
			    (dev->axes[a].max + dev->axes[a].min) / 2;
		}
		kprintf("[JOY] Calibration reset for device %d\n", joy_idx);
		return 0;
	}

	default:
		return -ENOTTY;
	}
}
