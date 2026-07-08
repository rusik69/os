/*
 * src/drivers/virtio_packed.c — VirtIO 1.1 packed virtqueue support
 *
 * Implements the packed virtqueue format introduced in VirtIO 1.1 (spec
 * section 2.8).  Unlike the traditional split virtqueue (descriptor table +
 * available ring + used ring), packed virtqueues use a single descriptor
 * ring with ownership tracked via wrap counters embedded in descriptor
 * flags.
 *
 * Usage:
 *   1. struct virtio_packed_vq vq;
 *   2. virtio_packed_vq_init(&vq, queue_idx, queue_size, vdev);
 *   3. virtio_packed_add_buf(&vq, phys_addr, len, write_flag);
 *   4. virtio_packed_kick(&vq);
 *   5. int desc_id = virtio_packed_get_buf(&vq, &len_out);
 *   6. virtio_packed_vq_cleanup(&vq);
 */

#include "types.h"
#include "virtio.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "pci.h"
#include "io.h"

/*
 * ── Memory layout of a packed virtqueue ───────────────────────────
 *
 *   [0]  desc[0]     16 bytes  (addr:8, len:4, flags:2, next:2)
 *   [16] desc[1]     16 bytes
 *   ...     ...
 *   [queue_size*16]  avail_event  2 bytes  (driver writes to suppress interrupts)
 *   [queue_size*16+2] used_event  2 bytes  (device sets to signal used bufs)
 *
 * The entire region must be contiguous and page-aligned.
 */

/* Standard descriptor flags (shared with split virtqueue format) */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

/* Number of bytes per descriptor in the packed ring */
#define PVIRTQ_DESC_SIZE        16u

/* Default queue size (must be power of 2) */
#define PVIRTQ_DEFAULT_SIZE     256

/* Maximum queue size */
#define PVIRTQ_MAX_SIZE         32768

/* ── Helper: extract avail/used wrap counter from descriptor flags ─ */
static inline int pv_avail_wrap(uint16_t flags)
{
	return (int)((flags >> 7) & 1u);
}

static inline int pv_used_wrap(uint16_t flags)
{
	return (int)((flags >> 15) & 1u);
}

/* ── virtio_packed_vq_size: compute required memory ────────────────
 * Returns the number of bytes needed (rounded up to page boundary).
 */
size_t virtio_packed_vq_size(uint16_t n)
{
	size_t needed;

	if (n < 2)
		n = 2;
	if (n > PVIRTQ_MAX_SIZE)
		n = PVIRTQ_MAX_SIZE;

	/* Descriptor ring + 4 bytes for event fields */
	needed = (size_t)n * PVIRTQ_DESC_SIZE + 4;

	/* Round up to page boundary (4096 = 0x1000) */
	needed = (needed + 0xFFFu) & ~(size_t)0xFFFu;

	return needed;
}

/* ── virtio_packed_vq_init: allocate and initialise a packed vq ────
 * If vdev is non-NULL, the queue is set up via the modern PCI transport.
 * Returns 0 on success, -1 on failure.
 */
int virtio_packed_vq_init(struct virtio_packed_vq *vq,
                          uint16_t queue_idx, uint16_t queue_size,
                          struct vpci_modern_device *vdev)
{
	size_t ring_sz;
	size_t num_pages;
	uint64_t ring_phys;
	uint64_t *alloc_result;

	if (!vq)
		return -1;

	memset(vq, 0, sizeof(*vq));

	/* Validate and clamp queue size */
	if (queue_size < 2)
		queue_size = 2;
	if (queue_size > PVIRTQ_MAX_SIZE)
		queue_size = PVIRTQ_MAX_SIZE;

	/* Round down to power of 2 */
	{
		uint16_t p2 = 1;
		while (p2 < queue_size)
			p2 <<= 1;
		queue_size = p2;
	}

	vq->queue_idx   = queue_idx;
	vq->queue_size   = queue_size;
	vq->avail_wrap   = 1;
	vq->used_wrap    = 1;
	vq->next_idx     = 0;
	vq->last_used    = 0;
	vq->free_count   = queue_size;
	vq->modern       = (vdev != NULL) ? 1 : 0;
	vq->vdev         = vdev;

	/* Allocate queue memory (physically contiguous, page-aligned) */
	ring_sz = virtio_packed_vq_size(queue_size);
	num_pages = ring_sz / 4096;
	if (num_pages < 1)
		num_pages = 1;

	alloc_result = pmm_alloc_frames(num_pages);
	if (!alloc_result) {
		kprintf("[VIRTIO-PACKED] failed to allocate %llu pages for queue %u\n",
		        (unsigned long long)num_pages, (unsigned int)queue_idx);
		return -1;
	}

	/* pmm_alloc_frames returns the physical address of the first frame
	 * cast as uint64_t pointer.  The frames are guaranteed contiguous. */
	ring_phys = (uint64_t)(uintptr_t)alloc_result;
	vq->mem_phys = ring_phys;
	vq->mem_size = ring_sz;

	/* Map to virtual address via identity mapping */
	vq->mem = (void *)PHYS_TO_VIRT(ring_phys);
	if (!vq->mem) {
		kprintf("[VIRTIO-PACKED] PHYS_TO_VIRT failed for queue %u\n",
		        (unsigned int)queue_idx);
		pmm_free_frames_contiguous(ring_phys, num_pages);
		return -1;
	}

	/* Zero the ring memory */
	memset(vq->mem, 0, ring_sz);

	/* Set up ring pointer */
	vq->ring = (struct pvirtq *)vq->mem;

	/* Register with the PCI transport */
	if (vq->modern && vq->vdev) {
		if (virtio_pci_modern_setup_packed_queue(
		        vq->vdev, vq->queue_idx, vq->queue_size,
		        vq->mem_phys) < 0) {
			kprintf("[VIRTIO-PACKED] failed to register queue %u "
			        "with modern transport\n",
			        (unsigned int)queue_idx);
			pmm_free_frames_contiguous(ring_phys, num_pages);
			vq->mem = NULL;
			vq->mem_phys = 0;
			return -1;
		}
	}

	return 0;
}

/* ── virtio_packed_vq_cleanup: tear down a packed virtqueue ─────── */
void virtio_packed_vq_cleanup(struct virtio_packed_vq *vq)
{
	size_t num_pages;

	if (!vq || !vq->mem)
		return;

	num_pages = vq->mem_size / 4096;
	if (num_pages < 1)
		num_pages = 1;

	pmm_free_frames_contiguous(vq->mem_phys, num_pages);
	memset(vq, 0, sizeof(*vq));
}

/* ── virtio_packed_add_buf: add a single descriptor to the ring ────
 * Returns 0 on success, -1 if the ring is full.
 */
int virtio_packed_add_buf(struct virtio_packed_vq *vq,
                          uint64_t addr, uint32_t len, int write)
{
	struct pvirtq_desc *desc;
	uint16_t idx;
	uint16_t flags;

	if (!vq || !vq->ring || vq->free_count == 0)
		return -1;

	idx = vq->next_idx;
	desc = &vq->ring->desc[idx];

	/* Build descriptor flags */
	flags = 0;
	if (write)
		flags |= VRING_DESC_F_WRITE;
	if (vq->avail_wrap)
		flags |= PVIRTQ_DESC_F_AVAIL;
	/* No chaining for single-buf add */

	desc->addr = addr;
	desc->len  = len;
	desc->flags = flags;
	desc->next = 0;

	/* Memory barrier: descriptor must be visible before advancing index */
	__asm__ volatile("" ::: "memory");

	/* Advance */
	vq->next_idx = (uint16_t)((idx + 1) & (vq->queue_size - 1));
	vq->free_count--;

	/* Toggle avail_wrap when we wrap around */
	if (vq->next_idx == 0)
		vq->avail_wrap ^= 1;

	return 0;
}

/* ── virtio_packed_add_buf_sg: add a scatter-gather chain ───────── */
int virtio_packed_add_buf_sg(struct virtio_packed_vq *vq,
                             const uint64_t *addrs, const uint32_t *lens,
                             const uint16_t *flags_in, int n)
{
	struct pvirtq_desc *desc;
	uint16_t head_idx;
	uint16_t idx;
	int i;

	if (!vq || !vq->ring || !addrs || !lens || !flags_in || n < 1)
		return -1;
	if ((uint16_t)n > vq->free_count)
		return -1;

	head_idx = vq->next_idx;
	idx = head_idx;

	for (i = 0; i < n; i++) {
		uint16_t this_flags = flags_in[i];
		int is_last = (i == n - 1);

		desc = &vq->ring->desc[idx];

		/* Add write flag if specified */
		if (this_flags & VRING_DESC_F_WRITE)
			desc->flags |= VRING_DESC_F_WRITE;

		/* Chain: every descriptor except the last has NEXT */
		if (!is_last)
			desc->flags |= VRING_DESC_F_NEXT;

		/* Set avail flag according to wrap counter */
		if (vq->avail_wrap)
			desc->flags |= PVIRTQ_DESC_F_AVAIL;

		desc->addr = addrs[i];
		desc->len  = lens[i];
		desc->next = (uint16_t)((idx + 1) & (vq->queue_size - 1));

		/* Advance to next descriptor */
		vq->next_idx = desc->next;
		vq->free_count--;

		/* Toggle avail_wrap when we wrap around */
		if (vq->next_idx == 0)
			vq->avail_wrap ^= 1;

		idx = vq->next_idx;
	}

	/* Memory barrier: descriptors must be visible before kick */
	__asm__ volatile("" ::: "memory");

	return 0;
}

/* ── virtio_packed_kick: notify the device of new buffers ────────── */
void virtio_packed_kick(struct virtio_packed_vq *vq)
{
	if (!vq)
		return;

	if (vq->modern && vq->vdev) {
		virtio_pci_modern_notify_queue(vq->vdev, vq->queue_idx);
	} else {
		/* Legacy PCI transport: write queue index to notify register */
		outw(VIRTIO_PCI_QUEUE_NOTIFY, vq->queue_idx);
	}
}

/* ── virtio_packed_get_buf: check for and retrieve a used buffer ───
 * Returns the descriptor index of a used buffer, or -1 if none.
 * If 'len_out' is non-NULL, it receives the length written by the device.
 */
int virtio_packed_get_buf(struct virtio_packed_vq *vq, uint32_t *len_out)
{
	struct pvirtq_desc *desc;
	uint16_t idx;
	int used_flag;
	int expected_wrap;
	int is_used;

	if (!vq || !vq->ring)
		return -1;

	idx = vq->last_used;
	desc = &vq->ring->desc[idx];

	/* Check if the descriptor's USED bit matches our wrap counter */
	used_flag = pv_used_wrap(desc->flags);
	expected_wrap = (int)vq->used_wrap;
	is_used = (used_flag == expected_wrap);

	if (!is_used)
		return -1;

	/* Memory barrier: read used-flag before reading len */
	__asm__ volatile("" ::: "memory");

	if (len_out)
		*len_out = desc->len;

	/* Return the descriptor index */
	if (desc->flags & VRING_DESC_F_NEXT) {
		/* For chained descriptors, return the head index */
		uint16_t ret_idx = idx;

		vq->last_used = (uint16_t)((idx + 1) & (vq->queue_size - 1));
		vq->free_count++;

		/* Toggle used_wrap on wraparound */
		if (vq->last_used == 0)
			vq->used_wrap ^= 1;

		return (int)ret_idx;
	}

	vq->last_used = (uint16_t)((idx + 1) & (vq->queue_size - 1));
	vq->free_count++;

	/* Toggle used_wrap on wraparound */
	if (vq->last_used == 0)
		vq->used_wrap ^= 1;

	return (int)idx;
}

/* ── virtio_packed_wait_buf: poll until a buffer completes ─────────
 * Spins up to 'timeout' iterations.  Returns 0 on success, -1 on timeout.
 */
int virtio_packed_wait_buf(struct virtio_packed_vq *vq, uint32_t timeout)
{
	uint32_t spin;

	if (!vq)
		return -1;

	spin = 0;
	while (virtio_packed_get_buf(vq, NULL) < 0) {
		__asm__ volatile("pause");
		if (++spin >= timeout) {
			return -1;
		}
	}

	return 0;
}

/* ── Enable event suppression for this queue ──────────────────────
 * Writes the current used_wrap + USED flag pattern to the avail_event
 * field so the device suppresses notifications until the wrap changes.
 */
void virtio_packed_enable_event(struct virtio_packed_vq *vq)
{
	volatile uint16_t *avail_event;

	if (!vq || !vq->ring)
		return;

	/*
	 * The avail_event is at offset queue_size * sizeof(struct pvirtq_desc)
	 * in the packed ring memory.
	 */
	avail_event = (volatile uint16_t *)((volatile uint8_t *)vq->ring
	               + (size_t)vq->queue_size * PVIRTQ_DESC_SIZE);

	/*
	 * We write the USED flag pattern that tells the device: do not
	 * notify us unless the used_wrap toggles (i.e. the queue wraps).
	 * The device will check: (flags & (1<<15)) == avail_event
	 */
	*avail_event = (uint16_t)(vq->used_wrap << 15);

	__asm__ volatile("" ::: "memory");
}

/* ── Disable event suppression ────────────────────────────────────
 * Writes ~(1<<15) to avail_event so the device always generates an
 * interrupt when it uses a descriptor.
 */
void virtio_packed_disable_event(struct virtio_packed_vq *vq)
{
	volatile uint16_t *avail_event;

	if (!vq || !vq->ring)
		return;

	avail_event = (volatile uint16_t *)((volatile uint8_t *)vq->ring
	               + (size_t)vq->queue_size * PVIRTQ_DESC_SIZE);

	/*
	 * Write the opposite of any valid USED bit pattern so the device
	 * always fires an interrupt on descriptor use.
	 */
	*avail_event = (uint16_t)((~(uint16_t)(vq->used_wrap << 15)) & 0xFFFFu);

	__asm__ volatile("" ::: "memory");
}

/*
 * ── Module init/exit stubs ────────────────────────────────────────
 * When built as a loadable module, these provide the standard hooks.
 */
#ifdef MODULE
int __init init_module(void)
{
	kprintf("[VIRTIO-PACKED] VirtIO 1.1 packed virtqueue support loaded\n");
	return 0;
}

void __exit cleanup_module(void)
{
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO 1.1 packed virtqueue support (Item 2: D147)");
MODULE_VERSION("1.0");
#endif /* MODULE */
