/*
 * src/drivers/virtio_balloon.c — VirtIO Balloon driver
 *
 * Implements the VirtIO memory balloon device (PCI vendor 0x1AF4,
 * device 0x1002) with free page reporting (VIRTIO_BALLOON_F_REPORTING)
 * and page poisoning hint (VIRTIO_BALLOON_F_PAGE_POISON).
 *
 * The balloon device permits the host to reclaim guest memory: inflating
 * the balloon pins physical pages on the guest side, returning them to
 * the host for reuse; deflating does the reverse.  Free page reporting
 * extends this by letting the guest proactively inform the host which
 * pages are free, which improves live-migration efficiency and host
 * memory overcommit.  Page poisoning hint communicates the poison value
 * this kernel uses for freed pages so the host can preserve it across
 * balloon operations, enabling use-after-free detection on the host.
 *
 * Layout of virtqueues (indices):
 *   0 – inflateq       guest → host (pages the guest gives up)
 *   1 – deflateq       host → guest (pages returned to guest)
 *   2 – statsq         optional memory statistics
 *   3 – reporting_vq   free-page reporting (VIRTIO_BALLOON_F_REPORTING)
 *
 * Feature bits negotiated:
 *   VIRTIO_BALLOON_F_MUST_TELL_HOST    (0) – must notify before using page
 *   VIRTIO_BALLOON_F_STATS_VQ          (1) – memory statistics virtqueue
 *   VIRTIO_BALLOON_F_DEFLATE_ON_OOM    (2) – deflate when OOM
 *   VIRTIO_BALLOON_F_PAGE_POISON       (4) – page poisoning hint
 *   VIRTIO_BALLOON_F_REPORTING         (5) – free page reporting
 *
 * Uses the legacy PCI I/O transport pattern (matching virtio_rng).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "pmm.h"
#include "heap.h"
#include "errno.h"
#include "page_poison.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ──────────────────────────────────────────────────── */

#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_BALLOON_DEVICE   0x1002

/* ── Feature bits (spec §5.5.6) ──────────────────────────────────── */

#define VIRTIO_BALLOON_F_MUST_TELL_HOST  (1u << 0)
#define VIRTIO_BALLOON_F_STATS_VQ        (1u << 1)
#define VIRTIO_BALLOON_F_DEFLATE_ON_OOM  (1u << 2)
#define VIRTIO_BALLOON_F_FREE_PAGE_HINT  (1u << 3)
#define VIRTIO_BALLOON_F_PAGE_POISON     (1u << 4)
#define VIRTIO_BALLOON_F_REPORTING       (1u << 5)

/* ── Ring constants ──────────────────────────────────────────────── */

#define VRING_SIZE             256
#define QUEUE_MEM_SIZE         16384

/* ── Descriptor flags (split virtqueue) ──────────────────────────── */

#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

/* ── Balloon device-specific config registers (legacy I/O port,
 *     standard virtio registers occupy 0x00–0x13; device config
 *     starts at 0x14 per the spec ─────────────────────────────── */

#define BALLOON_CFG_BASE       0x14u
#define BALLOON_CFG_NUM_PAGES  (BALLOON_CFG_BASE + 0u)   /* RO: current balloon size */
#define BALLOON_CFG_ACTUAL     (BALLOON_CFG_BASE + 4u)   /* WO: actual balloon size */
#define BALLOON_CFG_FREE_CMD   (BALLOON_CFG_BASE + 8u)   /* RW: free-page-hint cmd ID */
#define BALLOON_CFG_POISON     (BALLOON_CFG_BASE + 12u)  /* WO: page poison value */

/* ── Free-page reporting constants ────────────────────────────────── */

#define FREE_CMD_ID_STOP       0
#define FREE_CMD_ID_MAX        0x800

/* Max page chunks per report submission (sized to fit in a single
 * descriptor chain without overflowing VRING_SIZE). */
#define MAX_CHUNKS_PER_REPORT  64

/* ── Split virtqueue ring structures (legacy layout) ─────────────── */

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

/* ── Free-page report structures (VirtIO spec §5.5.6.5) ──────────── */

#pragma pack(push, 1)
struct free_page_report_hdr {
	uint16_t cmd;
	uint16_t padding[3];
};

struct page_chunk {
	uint64_t base_addr;          /* physical address of the first page */
	uint16_t size;              /* number of contiguous 4K pages */
	uint16_t padding[3];
};
#pragma pack(pop)

/* ── Per-virtqueue state ──────────────────────────────────────────── */

struct vbq {
	uint8_t  mem[QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
	int      initialized;

	uint16_t queue_idx;

	struct vring_desc  *descs;
	struct vring_avail *avail;
	struct vring_used  *used;

	uint16_t last_used_idx;

	/* Tracking for inflate/deflate (balloon pages) */
	uint64_t *pinned_pages;      /* physical addresses of pinned pages */
	uint32_t  num_pinned;        /* count of currently pinned pages   */
	uint32_t  max_pinned;        /* capacity of pinned_pages[]        */
};

/* ── Driver global state ──────────────────────────────────────────── */

static int        balloon_present  = 0;
static uint16_t   balloon_iobase   = 0;
static uint32_t   balloon_features = 0;

static struct vbq inflate_q;       /* vq 0 */
static struct vbq deflate_q;       /* vq 1 */
static struct vbq stats_q;         /* vq 2 (VIRTIO_BALLOON_F_STATS_VQ) */
static struct vbq reporting_q;     /* vq 3 (VIRTIO_BALLOON_F_REPORTING) */

/* Pre-allocated report buffer for free-page reporting (must be
 * physically addressable for device DMA). */
static struct free_page_report_hdr report_hdr __attribute__((aligned(16)));
static struct page_chunk           report_chunk __attribute__((aligned(16)));

/* ── I/O port helpers (legacy transport) ──────────────────────────── */

static inline void vbl_outb(uint8_t off, uint8_t v)
{
	outb(balloon_iobase + off, v);
}

static inline void vbl_outw(uint8_t off, uint16_t v)
{
	outw(balloon_iobase + off, v);
}

static inline void vbl_outl(uint8_t off, uint32_t v)
{
	outb((uint16_t)(balloon_iobase + off),     (uint8_t)(v));
	outb((uint16_t)(balloon_iobase + off + 1), (uint8_t)(v >> 8));
	outb((uint16_t)(balloon_iobase + off + 2), (uint8_t)(v >> 16));
	outb((uint16_t)(balloon_iobase + off + 3), (uint8_t)(v >> 24));
}

static inline uint8_t vbl_inb(uint8_t off)
{
	return inb(balloon_iobase + off);
}

static inline uint16_t vbl_inw(uint8_t off)
{
	return inw(balloon_iobase + off);
}

static inline uint32_t vbl_inl(uint8_t off)
{
	return (uint32_t)inb((uint16_t)(balloon_iobase + off)) |
	       ((uint32_t)inb((uint16_t)(balloon_iobase + off + 1)) << 8)  |
	       ((uint32_t)inb((uint16_t)(balloon_iobase + off + 2)) << 16) |
	       ((uint32_t)inb((uint16_t)(balloon_iobase + off + 3)) << 24);
}

/* ── Device config accessors ──────────────────────────────────────── */

static uint32_t vbl_read_config(uint8_t offset)
{
	return vbl_inl(offset);
}

static void vbl_write_config(uint8_t offset, uint32_t val)
{
	vbl_outl(offset, val);
}

/* ── Virtqueue helpers ────────────────────────────────────────────── */

static void vbl_select_queue(uint16_t idx)
{
	vbl_outw(VIRTIO_PCI_QUEUE_SEL, idx);
}

static void vbl_notify_queue(uint16_t idx)
{
	vbl_select_queue(idx);
	vbl_outw(VIRTIO_PCI_QUEUE_NOTIFY, idx);
}

/**
 * vbl_init_vq – initialise a single split virtqueue
 * @q:          per-queue state struct
 * @queue_idx:  device queue index
 *
 * Selects the queue, writes the physical page number of the queue memory
 * into the device, and wires up the descriptor/avail/used ring pointers.
 */
static void vbl_init_vq(struct vbq *q, uint16_t queue_idx)
{
	memset(q, 0, sizeof(*q));
	q->queue_idx = queue_idx;

	vbl_select_queue(queue_idx);

	/* Point the device at our queue memory (physical page number). */
	uint64_t phys = (uint64_t)(uintptr_t)q->mem;
	vbl_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

	/* Set up ring pointers into the queue memory buffer. */
	q->descs = (struct vring_desc *)q->mem;
	q->avail = (struct vring_avail *)(q->mem +
		   sizeof(struct vring_desc) * VRING_SIZE);
	q->used  = (struct vring_used *)(q->mem + 2048);
	q->last_used_idx = 0;
	q->initialized   = 1;
}

/**
 * vbl_submit_and_wait – submit a single descriptor and busy-wait
 * @q:     virtqueue state
 * @addr:  guest-physical address of the buffer
 * @len:   buffer length in bytes
 * @flags: descriptor flags (VRING_DESC_F_WRITE if device writes)
 *
 * Returns 0 on success, -1 on timeout.
 */
static int vbl_submit_and_wait(struct vbq *q, uint64_t addr,
				uint32_t len, uint16_t flags)
{
	struct vring_desc  *descs = q->descs;
	struct vring_avail *avail = q->avail;
	struct vring_used  *used  = q->used;

	descs[0].addr  = addr;
	descs[0].len   = len;
	descs[0].flags = flags;
	descs[0].next  = 0;

	uint16_t prev_used = used->idx;
	uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
	avail->ring[avail_idx] = 0;
	__asm__ volatile("" ::: "memory");
	avail->idx++;
	__asm__ volatile("" ::: "memory");

	vbl_notify_queue(q->queue_idx);

	uint32_t timeout = 100000;
	while (used->idx == prev_used && timeout--) {
		__asm__ volatile("pause");
	}

	if (timeout == 0) {
		kprintf("[VIRTIO-BALLOON] timeout on queue %u\n",
			(unsigned int)q->queue_idx);
		return -1;
	}

	return 0;
}

/**
 * vbl_submit_chain_and_wait – submit a descriptor chain and busy-wait
 * @q:     virtqueue state
 * @addrs: array of guest-physical addresses (length @n)
 * @lens:  array of buffer lengths   (length @n)
 * @flags: array of descriptor flags (length @n)
 * @n:     number of descriptors in the chain
 *
 * Returns 0 on success, -1 on timeout.
 */
static int vbl_submit_chain_and_wait(struct vbq *q,
				      const uint64_t *addrs,
				      const uint32_t *lens,
				      const uint16_t *flags,
				      int n)
{
	struct vring_desc  *descs = q->descs;
	struct vring_avail *avail = q->avail;
	struct vring_used  *used  = q->used;
	int i;

	if (n <= 0 || n > VRING_SIZE)
		return -1;

	for (i = 0; i < n; i++) {
		descs[i].addr  = addrs[i];
		descs[i].len   = lens[i];
		descs[i].flags = flags[i];
		descs[i].next  = (uint16_t)((i < n - 1) ? (i + 1) : 0);
	}

	uint16_t prev_used = used->idx;
	uint16_t avail_idx = avail->idx & (VRING_SIZE - 1);
	avail->ring[avail_idx] = 0;
	__asm__ volatile("" ::: "memory");
	avail->idx++;
	__asm__ volatile("" ::: "memory");

	vbl_notify_queue(q->queue_idx);

	uint32_t timeout = 100000;
	while (used->idx == prev_used && timeout--) {
		__asm__ volatile("pause");
	}

	if (timeout == 0) {
		kprintf("[VIRTIO-BALLOON] timeout on chain queue %u\n",
			(unsigned int)q->queue_idx);
		return -1;
	}

	return 0;
}

/* ── Inflate – pin frames and report to host ──────────────────────── */

/**
 * vbl_inflate – allocate @count physical frames and submit them
 *               on the inflate virtqueue
 *
 * Returns the number of frames successfully inflated.
 */
static uint32_t vbl_inflate(struct vbq *q, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		uint64_t frame = pmm_alloc_frame();
		if (!frame)
			break;

		uint64_t phys = frame << 12;
		if (vbl_submit_and_wait(q, phys, 4096, 0) < 0) {
			pmm_free_frame(frame);
			break;
		}

		if (q->num_pinned < q->max_pinned) {
			q->pinned_pages[q->num_pinned] = phys;
			q->num_pinned++;
		}
	}

	return i;
}

/* ── Deflate – release pinned frames back to the system ──────────── */

/**
 * vbl_deflate – release up to @count pinned frames, informing the
 *               host via the deflate virtqueue
 *
 * Returns the number of frames successfully deflated.
 */
static uint32_t vbl_deflate(struct vbq *q, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count && q->num_pinned > 0; i++) {
		q->num_pinned--;
		uint64_t phys = q->pinned_pages[q->num_pinned];

		if (vbl_submit_and_wait(q, phys, 4096, 0) < 0) {
			q->num_pinned++; /* restore on failure */
			break;
		}

		pmm_free_frame((uint64_t)(phys >> 12));
	}

	return i;
}

/* ── Free page reporting ──────────────────────────────────────────── */

/**
 * vbl_report_free_pages – scan the PMM for free regions and submit
 *                          free-page reports on the reporting vq
 *
 * Iterates through physically-contiguous free regions using
 * pmm_find_free_region() and submits each as a (header, chunk) pair
 * to the reporting virtqueue.  The host acknowledges each report via
 * the used ring.
 *
 * Returns the total number of pages reported, or 0 if the host has
 * disabled reporting or no free regions exist.
 */
static uint32_t vbl_report_free_pages(struct vbq *rq)
{
	uint32_t total_reported = 0;
	uint64_t start_frame;
	uint64_t region_count;
	uint32_t cmd_id;
	uint32_t chunk_count;

	cmd_id = vbl_read_config(BALLOON_CFG_FREE_CMD);
	if (cmd_id == FREE_CMD_ID_STOP)
		return 0;

	start_frame = 0;
	chunk_count = 0;

	while (chunk_count < MAX_CHUNKS_PER_REPORT) {
		start_frame = pmm_find_free_region(start_frame,
						    &region_count);
		if (start_frame == ~0ULL || region_count == 0)
			break;

		if (region_count > 0xFFFFu)
			region_count = 0xFFFFu;

		/* Prepare the report header with the current command ID
		 * from the device config. */
		report_hdr.cmd       = (uint16_t)cmd_id;
		report_hdr.padding[0] = 0;
		report_hdr.padding[1] = 0;
		report_hdr.padding[2] = 0;

		/* Prepare the page-chunk descriptor.  The chunk's
		 * base_addr is the guest-physical address of the first
		 * page of this free region. */
		report_chunk.base_addr = start_frame * 4096;
		report_chunk.size      = (uint16_t)region_count;
		report_chunk.padding[0] = 0;
		report_chunk.padding[1] = 0;
		report_chunk.padding[2] = 0;

		/* Build a two-descriptor chain:
		 *   [0] report header   (device-readable — guest writes)
		 *   [1] page chunk      (device-readable — guest writes)
		 *
		 * The device reads both, acknowledges via the used ring.
		 */
		uint64_t addrs[2];
		uint32_t lens[2];
		uint16_t flags[2];

		addrs[0] = VIRT_TO_PHYS(&report_hdr);
		lens[0]  = sizeof(report_hdr);
		flags[0] = 0;  /* device reads this data */

		addrs[1] = VIRT_TO_PHYS(&report_chunk);
		lens[1]  = sizeof(report_chunk);
		flags[1] = 0;  /* device reads this data */

		if (vbl_submit_chain_and_wait(rq, addrs, lens,
					       flags, 2) < 0)
			break;

		total_reported += (uint32_t)region_count;
		start_frame    += region_count;
		chunk_count++;

		/* Re-check command ID — host may ask us to stop. */
		cmd_id = vbl_read_config(BALLOON_CFG_FREE_CMD);
		if (cmd_id == FREE_CMD_ID_STOP)
			break;
	}

	return total_reported;
}

/* ── Queue initialisation ─────────────────────────────────────────── */

static void vbl_init_queues(void)
{
	inflate_q.pinned_pages = NULL;
	inflate_q.num_pinned   = 0;
	inflate_q.max_pinned   = 0;
	vbl_init_vq(&inflate_q, 0);
	vbl_init_vq(&deflate_q, 1);

	if (balloon_features & VIRTIO_BALLOON_F_STATS_VQ)
		vbl_init_vq(&stats_q, 2);

	if (balloon_features & VIRTIO_BALLOON_F_REPORTING)
		vbl_init_vq(&reporting_q, 3);
}

/* ── Initialise the balloon device ────────────────────────────────── */

static void virtio_balloon_init(void)
{
	struct pci_device dev;
	int ret;

	if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BALLOON_DEVICE,
			    &dev) < 0) {
		kprintf("[VIRTIO-BALLOON] device not found\n");
		return;
	}

	balloon_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
	if (!balloon_iobase) {
		kprintf("[VIRTIO-BALLOON] no I/O base\n");
		return;
	}

	pci_enable_bus_master(&dev);

	/* Reset + acknowledge + driver. */
	vbl_outb(VIRTIO_PCI_STATUS, 0);
	vbl_outb(VIRTIO_PCI_STATUS,
		 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	/* Supported features:
	 *   MUST_TELL_HOST – required for correct inflate/deflate
	 *   DEFLATE_ON_OOM – deflate balloon when guest runs low on mem
	 *   STATS_VQ       – memory statistics (optional, wire it up)
	 *   PAGE_POISON    – page poisoning hint (tell host our poison value)
	 *   REPORTING      – free page reporting (the main addition)
	 */
	uint32_t supported = VIRTIO_BALLOON_F_MUST_TELL_HOST |
			     VIRTIO_BALLOON_F_DEFLATE_ON_OOM |
			     VIRTIO_BALLOON_F_STATS_VQ |
			     VIRTIO_BALLOON_F_PAGE_POISON |
			     VIRTIO_BALLOON_F_REPORTING;

	ret = virtio_negotiate_features_ex(vbl_inl, vbl_outl,
					   vbl_outb, vbl_inb,
					   supported, 0, NULL,
					   "virtio-balloon");
	if (ret < 0) {
		kprintf("[VIRTIO-BALLOON] feature negotiation failed\n");
		return;
	}

	/* Read back negotiated features. */
	vbl_select_queue(0);
	balloon_features = vbl_inl(VIRTIO_PCI_GUEST_FEAT);

	/* If page poisoning hint was negotiated, tell the host what
	 * poison value we use for freed pages.  This allows the host
	 * to preserve the poison pattern across balloon operations so
	 * the guest can detect use-after-free on returned pages. */
	if (balloon_features & VIRTIO_BALLOON_F_PAGE_POISON) {
		uint8_t poison_val = page_poison_get_freed_value();
		vbl_write_config(BALLOON_CFG_POISON, (uint32_t)poison_val);
		kprintf("[VIRTIO-BALLOON] page poison hint: 0x%02x\n",
			(unsigned int)poison_val);
	}

	/* Initialise the virtqueues. */
	vbl_init_queues();

	/* Tell the device our actual balloon size is 0. */
	vbl_write_config(BALLOON_CFG_ACTUAL, 0);

	/* Driver OK — device is live. */
	vbl_outb(VIRTIO_PCI_STATUS,
		 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		 VIRTIO_STATUS_DRIVER_OK);

	balloon_present = 1;

	kprintf("[VIRTIO-BALLOON] VirtIO Balloon at %02x:%02x.%d,"
		" I/O 0x%04x, features 0x%08X\n",
		dev.bus, dev.slot, dev.func,
		balloon_iobase, (unsigned int)balloon_features);

	/* If free page reporting was negotiated, send an initial
	 * report so the host immediately learns which pages are free. */
	if (balloon_features & VIRTIO_BALLOON_F_REPORTING) {
		uint32_t reported = vbl_report_free_pages(&reporting_q);
		kprintf("[VIRTIO-BALLOON] initial free page report:"
			" %u pages (%u chunks)\n",
			(unsigned int)reported,
			(unsigned int)(reported > 0 ? 1 : 0));
	}
}

#ifdef MODULE
int __init init_module(void)
{
	virtio_balloon_init();
	return 0;
}

void __exit cleanup_module(void)
{
	/* Nothing to clean up for now. */
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO balloon — inflate/deflate, free page reporting, page poisoning hint");
MODULE_VERSION("2.1");
#endif
