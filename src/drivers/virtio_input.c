/*
 * src/drivers/virtio_input.c — VirtIO input driver with multi-touch support
 *
 * Implements VirtIO input (keyboard, mouse, touchscreen, tablet)
 * for PCI device 1AF4:1052 (virtio-input).  Supports:
 *   - Event virtqueue (vq 0) for receiving input events
 *   - Status virtqueue (vq 1) for sending feedback events
 *   - VirtIO input configuration for capability discovery
 *   - Multi-touch protocol type B with slot tracking
 *   - Keyboard, mouse, and touch event forwarding
 *
 * Multi-touch protocol (type B):
 *   The device sends ABS_MT_SLOT to select a slot, then ABS_MT_TRACKING_ID
 *   to begin/end/update a contact.  Position and other properties follow,
 *   terminated by EV_SYN / SYN_REPORT.
 *
 * References:
 *   VirtIO specification v1.2, §5.10 — Input Device
 *   Linux kernel input documentation for multi-touch protocol type B
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_INPUT_DEVICE    0x1052

/* ── Input event types (from virtio-input spec) ────────────────── */

#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define SYN_REPORT      0

/* ── Ring constants ────────────────────────────────────────────── */

#define VRING_SIZE             64   /* enough for multi-touch event bursts */
#define QUEUE_MEM_SIZE         8192

/* Descriptor flags */
#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

/* ── Multi-touch constants ──────────────────────────────────────── */

#define MT_MAX_SLOTS           10   /* max concurrent touch points */

/* ── Internal event buffer (consumed by virtio_input_read) ──────── */

#define INPUT_EVENT_BUF_SIZE   256

/* ── Split virtqueue ring structures (legacy layout) ───────────── */
/* Legacy VirtIO requires the used ring to be page-aligned (4K).    */

#define VIRTIO_PCI_VRING_ALIGN  4096

#pragma pack(push, 1)
struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[VRING_SIZE];
};

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[VRING_SIZE];
};
#pragma pack(pop)

/* ── Touch contact slot (multi-touch protocol type B) ──────────── */

struct vi_contact {
	int      active;          /* 1 if slot contains live data */
	int      tracking_id;     /* -1 if slot is inactive */
	uint32_t x;               /* X coordinate in device units */
	uint32_t y;               /* Y coordinate in device units */
	uint32_t pressure;        /* touch pressure */
	uint32_t touch_major;     /* major axis of touch ellipse */
	uint32_t touch_minor;     /* minor axis of touch ellipse */
	uint32_t width_major;     /* major axis of approaching ellipse */
	uint32_t width_minor;     /* minor axis of approaching ellipse */
	int      orientation;     /* ellipse orientation (-127..127) */
};

/* ── Per-virtqueue state ──────────────────────────────────────── */

struct vi_vq {
	uint8_t  mem[QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
	int      initialized;

	uint16_t queue_idx;

	struct vring_desc  *descs;
	struct vring_avail *avail;
	struct vring_used  *used;

	uint16_t last_used_idx;
};

/* ── Device state ──────────────────────────────────────────────── */

static int              input_present = 0;
static uint16_t         input_iobase  = 0;
static uint32_t         input_features = 0;
static struct pci_device input_pci_dev;

/* Event virtqueue (index 0) — device sends events on this */
static struct vi_vq event_vq;

/* Status virtqueue (index 1) — driver sends feedback/leds */
static struct vi_vq status_vq;

/* ── Multi-touch state ─────────────────────────────────────────── */

static struct vi_contact mt_contacts[MT_MAX_SLOTS];
static int               mt_current_slot;    /* last ABS_MT_SLOT received */
static int               mt_num_contacts;    /* active contact count */

/* ── Capability bitmaps (from device config) ────────────────────── */

static uint8_t ev_bits[128 / 8];      /* supported event types (EV_*) */
static uint8_t abs_bits[256 / 8];     /* supported ABS codes (ABS_*) — loop iterates i < 256 */
static uint8_t key_bits[128 / 8];     /* supported key codes (KEY_*) */
static uint8_t rel_bits[128 / 8];     /* supported relative axes (REL_*) */
static uint8_t prop_bits[128 / 8];    /* device properties */

/* ── ABS axis info (for each supported ABS_ code) ──────────────── */

static struct virtio_input_absinfo abs_info[256];  /* indexed by ABS code */

/* ── Event ring buffer (for virtio_input_read) ──────────────────── */

static struct virtio_input_event event_buf[INPUT_EVENT_BUF_SIZE];
static volatile int event_head;   /* consumer index */
static volatile int event_tail;   /* producer index */
static spinlock_t   event_lock;

/* ── Event buffer descriptor (shared with device) ──────────────── */

/* We allocate 64 event-receive buffers and cycle through them.
 * Each holds a single virtio_input_event (8 bytes).
 * The device writes into these via DMA. */
#define NUM_EVENT_BUFS  64

static struct virtio_input_event event_pool[NUM_EVENT_BUFS]
	__attribute__((aligned(16)));

/* ── Forward declarations ────────────────────────────────────────── */

 static void vi_process_mt_event(const struct virtio_input_event *ev);
 static void vi_push_event(const struct virtio_input_event *ev);
 static void vi_process_events(struct vi_vq *q);
 static int  vi_submit_event_buf(struct vi_vq *q,
 				struct virtio_input_event *ev,
 				uint16_t desc_idx);

/* ── I/O helpers (legacy PCI transport) ──────────────────────────── */

static inline void vi_outb(uint8_t off, uint8_t v)
{
	outb(input_iobase + off, v);
}

static inline void vi_outw(uint8_t off, uint16_t v)
{
	outw(input_iobase + off, v);
}

static inline void vi_outl(uint8_t off, uint32_t v)
{
	outl((uint16_t)(input_iobase + off), v);
}

static inline uint8_t vi_inb(uint8_t off)
{
	return inb(input_iobase + off);
}

static inline uint16_t vi_inw(uint8_t off)
{
	return inw(input_iobase + off);
}

static inline uint32_t vi_inl(uint8_t off)
{
	return inl((uint16_t)(input_iobase + off));
}

/* ── Device config access (legacy PCI I/O port) ──────────────────── */

/*
 * Read a single byte from the virtio-input device configuration space.
 * The virtio-input config struct is accessed via the legacy I/O transport
 * by writing select/subsel then reading the value.
 *
 * For legacy transport, input config starts at I/O offset 0x14
 * (after the standard virtio PCI registers 0x00–0x13).
 */
#define VI_CFG_BASE    0x14u

static void vi_config_select(uint8_t select, uint8_t subsel)
{
	vi_outb(VI_CFG_BASE + 0, select);
	vi_outb(VI_CFG_BASE + 1, subsel);
	/* Read the size field (byte 2) — not needed for legacy access */
	(void)vi_inb(VI_CFG_BASE + 2);
}

static uint8_t vi_config_read_bits(struct virtio_input_config *cfg,
				   uint8_t select, uint8_t subsel)
{
	int i;
	int bytes;
	uint8_t size_val;

	memset(cfg, 0, sizeof(*cfg));

	vi_outb(VI_CFG_BASE + 0, select);   /* select register */
	vi_outb(VI_CFG_BASE + 1, subsel);   /* subsel register */
	__asm__ volatile("" ::: "memory");

	/* Read size (byte 2) to know how many bytes follow */
	size_val = vi_inb(VI_CFG_BASE + 2);

	/* The bits[] field starts at offset 8 within the config struct.
	 * Reading at offset 8 from VI_CFG_BASE gives us the u.bits[] array.
	 * The spec says the size indicates how many meaningful bytes. */
	if (size_val == 0 || size_val > 128)
		size_val = 128;

	bytes = (int)size_val;
	for (i = 0; i < bytes && i < 128; i++)
		cfg->u.bits[i] = vi_inb((uint8_t)(VI_CFG_BASE + 8 + i));

	return size_val;
}

static struct virtio_input_absinfo vi_config_read_abs(uint8_t code)
{
	struct virtio_input_absinfo info;
	uint8_t size_val;

	memset(&info, 0, sizeof(info));

	vi_outb(VI_CFG_BASE + 0, VIRTIO_INPUT_CFG_ABS_INFO);
	vi_outb(VI_CFG_BASE + 1, code);
	__asm__ volatile("" ::: "memory");

	size_val = vi_inb(VI_CFG_BASE + 2);

	/* The absinfo struct starts at offset 8 (20 bytes).
	 * Read min, max, fuzz, flat, res as 4-byte LE values. */
	if (size_val >= 4)
		info.min = (uint32_t)vi_inl((uint8_t)(VI_CFG_BASE + 8));
	if (size_val >= 8)
		info.max = (uint32_t)vi_inl((uint8_t)(VI_CFG_BASE + 12));
	if (size_val >= 12)
		info.fuzz = (uint32_t)vi_inl((uint8_t)(VI_CFG_BASE + 16));
	if (size_val >= 16)
		info.flat = (uint32_t)vi_inl((uint8_t)(VI_CFG_BASE + 20));
	if (size_val >= 20)
		info.res = (uint32_t)vi_inl((uint8_t)(VI_CFG_BASE + 24));

	/* Fill defaults for unspecified fields */
	if (size_val < 4)
	{ info.min = 0; info.max = 0; }
	if (size_val < 8)
		info.max = 0;
	if (size_val < 12)
		info.fuzz = 0;
	if (size_val < 16)
		info.flat = 0;
	if (size_val < 20)
		info.res = 0;

	return info;
}

/* ── Queue helpers ─────────────────────────────────────────────── */

static void vi_select_queue(uint16_t idx)
{
	vi_outw(VIRTIO_PCI_QUEUE_SEL, idx);
}

static void vi_notify_queue(uint16_t idx)
{
	vi_select_queue(idx);
	vi_outw(VIRTIO_PCI_QUEUE_NOTIFY, idx);
}

static int vi_init_vq(struct vi_vq *q, uint16_t queue_idx)
{
	memset(q, 0, sizeof(*q));
	q->queue_idx = queue_idx;

	vi_select_queue(queue_idx);

	/* Point the device at our queue memory (physical page number) */
	uint64_t phys = (uint64_t)(uintptr_t)q->mem;
	vi_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

	/* Set up ring pointers into the queue memory buffer */
	q->descs = (struct vring_desc *)q->mem;
	q->avail = (struct vring_avail *)(q->mem +
		   sizeof(struct vring_desc) * VRING_SIZE);

	/* The used ring must be page-aligned per the VirtIO legacy spec.
	 * Device computes: offset = ALIGN(desc_table_end + avail_size, 4096) */
	size_t avail_end = sizeof(struct vring_desc) * VRING_SIZE +
			   sizeof(struct vring_avail) +
			   VRING_SIZE * sizeof(uint16_t);
	size_t used_off = (avail_end + VIRTIO_PCI_VRING_ALIGN - 1) &
			  ~(VIRTIO_PCI_VRING_ALIGN - 1);
	q->used  = (struct vring_used *)(q->mem + used_off);
	q->last_used_idx = 0;
	q->initialized   = 1;

	return 0;
}

/*
 * Submit an event buffer to the event virtqueue so the device can
 * fill it with the next input event.
 */
static int vi_submit_event_buf(struct vi_vq *q,
			       struct virtio_input_event *ev,
			       uint16_t desc_idx)
{
	struct vring_desc  *descs;
	struct vring_avail *avail;
	uint16_t avail_idx;

	if (!q->initialized)
		return -1;

	descs = q->descs;
	avail = q->avail;

	/* Build a single descriptor: device writes event data.
	 * Each pool buffer gets its own descriptor index so the device
	 * can process them concurrently. */
	if (desc_idx >= VRING_SIZE)
		return -1;
	descs[desc_idx].addr  = (uint64_t)(uintptr_t)ev;
	descs[desc_idx].len   = (uint32_t)sizeof(*ev);
	descs[desc_idx].flags = VRING_DESC_F_WRITE;
	descs[desc_idx].next  = 0;

	/* Submit to the avail ring */
	avail_idx = avail->idx & (VRING_SIZE - 1);
	avail->ring[avail_idx] = desc_idx;
	__asm__ volatile("" ::: "memory");
	avail->idx++;
	__asm__ volatile("" ::: "memory");

	return 0;
}

/*
 * Submit all event pool buffers as receive buffers on the event vq.
 * Each pool buffer uses its array index as the descriptor index so
 * the device can process them concurrently.
 */
static int vi_submit_all_event_bufs(struct vi_vq *q)
{
	int i;

	for (i = 0; i < NUM_EVENT_BUFS; i++) {
		if (vi_submit_event_buf(q, &event_pool[i], (uint16_t)i) < 0)
			return -1;
	}

	/* Single notification after all buffers are queued */
	vi_notify_queue(q->queue_idx);
	return 0;
}

/*
 * Poll for completed (used) event buffers.  For each completed buffer,
 * if the device wrote an event, process it.  Then re-submit the buffer
 * so the device can fill it again.
 */
static void vi_process_events(struct vi_vq *q)
{
	struct vring_used  *used;
	struct vring_desc  *descs;
	struct vring_avail *avail;
	int processed = 0;

	if (!q->initialized)
		return;

	used  = q->used;
	descs = q->descs;
	avail = q->avail;

	while (used->idx != q->last_used_idx) {
		uint16_t used_idx = q->last_used_idx & (VRING_SIZE - 1);
		uint32_t desc_id = used->ring[used_idx].id;
		uint32_t len     = used->ring[used_idx].len;

		q->last_used_idx++;

		/* The descriptor index directly identifies the pool buffer.
		 * (We submit each pool buffer at its own descriptor index.) */
		if (desc_id < NUM_EVENT_BUFS && len >= sizeof(struct virtio_input_event)) {
			/* Process the event the device wrote */
			struct virtio_input_event ev = event_pool[desc_id];

			/* Re-submit this buffer for the next event,
			 * using its dedicated descriptor index. */
			descs[desc_id].addr  = (uint64_t)(uintptr_t)&event_pool[desc_id];
			descs[desc_id].len   = (uint32_t)sizeof(event_pool[desc_id]);
			descs[desc_id].flags = VRING_DESC_F_WRITE;
			descs[desc_id].next  = 0;

			uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
			avail->ring[avail_idx] = (uint16_t)desc_id;
			__asm__ volatile("" ::: "memory");
			avail->idx++;
			__asm__ volatile("" ::: "memory");

			/* Queue event for processing */
			vi_push_event(&ev);
		}

		processed++;

		/* Notify device only after batch to reduce MMIO writes */
		if (processed >= 8) {
			vi_notify_queue(q->queue_idx);
			processed = 0;
		}
	}

	if (processed > 0)
		vi_notify_queue(q->queue_idx);
}

/*
 * vi_push_event — add a raw virtio_input_event to the internal ring buffer
 * and trigger multi-touch processing for EV_ABS / EV_KEY / EV_SYN events.
 */
static void vi_push_event(const struct virtio_input_event *ev)
{
	int next_tail;

	if (!ev)
		return;

	spinlock_acquire(&event_lock);

	next_tail = (event_tail + 1) % INPUT_EVENT_BUF_SIZE;
	if (next_tail != event_head) {
		event_buf[event_tail] = *ev;
		event_tail = next_tail;
	}

	spinlock_release(&event_lock);

	/* Process multi-touch state update inline */
	vi_process_mt_event(ev);
}

/*
 * vi_process_mt_event — update multi-touch state based on an event.
 *
 * Implements multi-touch protocol type B (slot-based):
 *   1. ABS_MT_SLOT selects the current slot
 *   2. ABS_MT_TRACKING_ID -1 releases the slot, any other ID starts/updates
 *   3. ABS_MT_POSITION_X/Y, ABS_MT_PRESSURE, etc. update the slot properties
 *   4. EV_SYN / SYN_REPORT commits the current frame
 */
static void vi_process_mt_event(const struct virtio_input_event *ev)
{
	if (!ev)
		return;

	if (ev->type == EV_ABS) {
		switch (ev->code) {
		case ABS_MT_SLOT:
			if (ev->value < MT_MAX_SLOTS)
				mt_current_slot = (int)ev->value;
			break;

		case ABS_MT_TRACKING_ID: {
			int slot = mt_current_slot;
			int new_id = (int)ev->value;

			if (slot < 0 || slot >= MT_MAX_SLOTS)
				break;

			if (new_id < 0) {
				/* Release the slot */
				if (mt_contacts[slot].active) {
					mt_contacts[slot].active = 0;
					mt_contacts[slot].tracking_id = -1;
					if (mt_num_contacts > 0)
						mt_num_contacts--;
				}
			} else {
				/* Start or update a contact */
				if (!mt_contacts[slot].active) {
					mt_contacts[slot].active = 1;
					mt_num_contacts++;
				}
				mt_contacts[slot].tracking_id = new_id;
			}
			break;
		}

		case ABS_MT_POSITION_X:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].x = ev->value;
			break;

		case ABS_MT_POSITION_Y:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].y = ev->value;
			break;

		case ABS_MT_TOUCH_MAJOR:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].touch_major = ev->value;
			break;

		case ABS_MT_TOUCH_MINOR:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].touch_minor = ev->value;
			break;

		case ABS_MT_WIDTH_MAJOR:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].width_major = ev->value;
			break;

		case ABS_MT_WIDTH_MINOR:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].width_minor = ev->value;
			break;

		case ABS_MT_ORIENTATION:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].orientation = (int)ev->value;
			break;

		case ABS_MT_PRESSURE:
			if (mt_current_slot >= 0 && mt_current_slot < MT_MAX_SLOTS)
				mt_contacts[mt_current_slot].pressure = ev->value;
			break;

		default:
			break;
		}
	} else if (ev->type == EV_KEY) {
		/* Key events are passed through to the event buffer */
		/* (handled by the generic push-event mechanism) */
	} else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
		/* Frame complete — multi-touch state is coherent now.
		 * Could trigger a callback to higher layers here. */
	}
}

/* ── Read device capabilities ──────────────────────────────────── */

static void vi_read_capabilities(void)
{
	struct virtio_input_config cfg;
	int i;

	memset(&cfg, 0, sizeof(cfg));

	/* Read supported event types */
	vi_config_read_bits(&cfg, VIRTIO_INPUT_CFG_EV_BITS, EV_SYN);
	memcpy(ev_bits, cfg.u.bits, 16);

	/* Check EV_KEY support */
	if (ev_bits[EV_KEY / 8] & (1u << (EV_KEY % 8))) {
		vi_config_read_bits(&cfg, VIRTIO_INPUT_CFG_EV_BITS, EV_KEY);
		memcpy(key_bits, cfg.u.bits, 16);
		kprintf("[VIRTIO-INPUT] keyboard/button events supported\n");
	}

	/* Check EV_REL support */
	if (ev_bits[EV_REL / 8] & (1u << (EV_REL % 8))) {
		vi_config_read_bits(&cfg, VIRTIO_INPUT_CFG_EV_BITS, EV_REL);
		memcpy(rel_bits, cfg.u.bits, 16);
		kprintf("[VIRTIO-INPUT] relative events supported\n");
	}

	/* Check EV_ABS support and read ABS axis info */
	if (ev_bits[EV_ABS / 8] & (1u << (EV_ABS % 8))) {
		vi_config_read_bits(&cfg, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS);
		memcpy(abs_bits, cfg.u.bits, 16);

		kprintf("[VIRTIO-INPUT] absolute events supported\n");

		/* Read ABS axis info for all supported axes */
		for (i = 0; i < 256; i++) {
			if (abs_bits[i / 8] & (1u << (i % 8))) {
				abs_info[i] = vi_config_read_abs((uint8_t)i);
				kprintf("[VIRTIO-INPUT]  ABS[0x%02x]: "
					"min=%u max=%u fuzz=%u flat=%u res=%u\n",
					i,
					(unsigned int)abs_info[i].min,
					(unsigned int)abs_info[i].max,
					(unsigned int)abs_info[i].fuzz,
					(unsigned int)abs_info[i].flat,
					(unsigned int)abs_info[i].res);
			}
		}

		/* Check for multi-touch capability */
		for (i = ABS_MT_SLOT; i <= ABS_MT_TOOL_Y; i++) {
			if (abs_bits[i / 8] & (1u << (i % 8))) {
				kprintf("[VIRTIO-INPUT] multi-touch axis ABS_MT_0x%02x "
					"supported\n", i);
			}
		}
	}

	/* Read device properties */
	vi_config_read_bits(&cfg, VIRTIO_INPUT_CFG_PROP_BITS, 0);
	memcpy(prop_bits, cfg.u.bits, 16);

	if (prop_bits[0] & (1u << 0))
		kprintf("[VIRTIO-INPUT] device has pointer property\n");
	if (prop_bits[0] & (1u << 1))
		kprintf("[VIRTIO-INPUT] device has direct (touchscreen) property\n");
	if (prop_bits[0] & (1u << 4))
		kprintf("[VIRTIO-INPUT] device has button pad property\n");
}

/* ── Public API ───────────────────────────────────────────────── */

/*
 * virtio_input_has_event — returns 1 if a buffered event is available
 */
static int virtio_input_has_event(void)
{
	return event_head != event_tail;
}

/*
 * virtio_input_read_event — retrieve the next buffered input event
 * Returns 0 on success, -1 if no events available.
 */
static int virtio_input_read_event(struct virtio_input_event *ev)
{
	int ret = -1;

	if (!ev)
		return -EINVAL;

	spinlock_acquire(&event_lock);

	if (event_head != event_tail) {
		*ev = event_buf[event_head];
		event_head = (event_head + 1) % INPUT_EVENT_BUF_SIZE;
		ret = 0;
	}

	spinlock_release(&event_lock);

	return ret;
}

/*
 * virtio_input_get_contact — get the current state of a touch slot
 * Returns 0 on success, -1 if slot is empty or out of range.
 */
static int virtio_input_get_contact(int slot, struct vi_contact *out)
{
	if (!out || slot < 0 || slot >= MT_MAX_SLOTS)
		return -EINVAL;

	if (!mt_contacts[slot].active)
		return -1;

	*out = mt_contacts[slot];
	return 0;
}

/*
 * virtio_input_num_contacts — returns number of active touch points
 */
static int virtio_input_num_contacts(void)
{
	return mt_num_contacts;
}

/*
 * virtio_input_read — legacy stub: reads raw event bytes into buf
 * (compatible with existing callers)
 */
static int virtio_input_read(void *dev, void *buf, size_t count)
{
	struct virtio_input_event ev;

	(void)dev;

	if (!buf || count < sizeof(ev))
		return -EINVAL;

	if (virtio_input_read_event(&ev) < 0)
		return 0;

	memcpy(buf, &ev, sizeof(ev));
	return (int)sizeof(ev);
}

/*
 * virtio_input_ioctl — legacy stub for future ioctl support
 */
static int virtio_input_ioctl(void *dev, int cmd, void *arg)
{
	(void)dev;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

/*
 * virtio_input_poll — called periodically to collect events from device
 */
static void virtio_input_poll(void)
{
	if (!input_present || !event_vq.initialized)
		return;

	vi_process_events(&event_vq);
}

/* ── Init / probe ──────────────────────────────────────────────── */

static void virtio_input_init(void)
{
	struct pci_device dev;
	struct virtio_input_config cfg;

	memset(&dev, 0, sizeof(dev));
	memset(&cfg, 0, sizeof(cfg));

	if (pci_find_device(VIRTIO_VENDOR, VIRTIO_INPUT_DEVICE, &dev) < 0)
		return;

	input_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
	if (!input_iobase)
		return;

	input_pci_dev = dev;

	pci_enable_bus_master(&dev);

	/* Reset the device */
	vi_outb(VIRTIO_PCI_STATUS, 0);

	/* Acknowledge + driver */
	vi_outb(VIRTIO_PCI_STATUS,
		VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	/* Negotiate features using the enhanced negotiator */
	if (virtio_negotiate_features_ex(
		vi_inl, vi_outl, vi_outb, vi_inb,
		VIRTIO_INPUT_F_EVENTS | VIRTIO_INPUT_F_STATUS,
		VIRTIO_INPUT_F_EVENTS,
		NULL /* feat_table — no input-specific feature table yet */,
		"virtio-input") < 0) {
		kprintf("[VIRTIO-INPUT] feature negotiation failed\n");
		return;
	}

	input_features = vi_inl(VIRTIO_PCI_GUEST_FEAT);

	/* Read device capabilities before setting up queues */
	vi_read_capabilities();

	/* Initialize event virtqueue (queue 0) */
	if (vi_init_vq(&event_vq, 0) < 0) {
		kprintf("[VIRTIO-INPUT] event queue init failed\n");
		return;
	}

	/* Initialize status virtqueue (queue 1) if supported */
	if (input_features & VIRTIO_INPUT_F_STATUS) {
		if (vi_init_vq(&status_vq, 1) < 0) {
			kprintf("[VIRTIO-INPUT] status queue init failed\n");
			/* Non-fatal — continue without status queue */
			status_vq.initialized = 0;
		}
	}

	/* Set FEATURES_OK */
	vi_outb(VIRTIO_PCI_STATUS,
		VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		VIRTIO_STATUS_FEATURES_OK);

	/* Read back to verify feature negotiation was accepted */
	if (!(vi_inb(VIRTIO_PCI_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
		kprintf("[VIRTIO-INPUT] device rejected feature negotiation\n");
		return;
	}

	/* Driver OK — device is live */
	vi_outb(VIRTIO_PCI_STATUS,
		VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

	input_present = 1;

	/* Submit all event pool buffers as receive descriptors.
	 * Done after DRIVER_OK per VirtIO spec (driver must not notify
	 * before DRIVER_OK). */
	if (vi_submit_all_event_bufs(&event_vq) < 0) {
		kprintf("[VIRTIO-INPUT] event buffer submission failed\n");
		/* Non-fatal — device is already live */
	}

	/* Initialize multi-touch state */
	{
		int i;
		for (i = 0; i < MT_MAX_SLOTS; i++) {
			mt_contacts[i].active = 0;
			mt_contacts[i].tracking_id = -1;
		}
	}
	mt_current_slot = 0;
	mt_num_contacts = 0;

	/* Initialize event buffer */
	event_head = 0;
	event_tail = 0;
	spinlock_init(&event_lock);

	kprintf("[VIRTIO-INPUT] VirtIO input (multi-touch capable) "
		"at %02x:%02x.%d, I/O 0x%04x, event vq=%s%s\n",
		dev.bus, dev.slot, dev.func, input_iobase,
		event_vq.initialized ? "OK" : "FAIL",
		status_vq.initialized ? " status=OK" : "");

	/* Collect initial events */
	vi_process_events(&event_vq);
}

/* ── Module entry/exit ──────────────────────────────────────────── */

#ifdef MODULE
int __init init_module(void)
{
	virtio_input_init();
	return 0;
}

void __exit cleanup_module(void)
{
	input_present = 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO input — keyboard, mouse, touchscreen, "
		   "multi-touch events via protocol type B");
MODULE_VERSION("1.0");
#endif
