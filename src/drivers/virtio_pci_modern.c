/*
 * src/drivers/virtio_pci_modern.c — Modern VirtIO PCI transport (MMIO + MSI-X)
 *
 * Implements the VirtIO 1.0+ PCI transport layer using MMIO regions
 * discovered via vendor-specific PCI capabilities.  This replaces the
 * legacy I/O port interface (VIRTIO_PCI_HOST_FEAT at 0x00 etc.) with a
 * capability-based MMIO scheme that supports large BARs, MSI-X, and
 * per-queue notification offsets.
 *
 * Usage from a device driver:
 *   1. pci_find_device(VIRTIO_VENDOR, device_id, &pci_dev)
 *   2. virtio_pci_modern_probe(&pci_dev, &vdev)
 *   3. if (vdev.modern_found) virtio_pci_modern_map_bars(&vdev)
 *   4. virtio_pci_modern_init_device(...)
 *   5. virtio_pci_modern_setup_queue(...)
 *   6. virtio_pci_modern_notify_queue(...)
 *
 * The probe function checks for PCI revision >= 1 (transitional/modern
 * devices) and scans for virtio vendor-specific capabilities (ID 0x09).
 * Legacy devices (rev 0) fall through gracefully.
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "apic.h"

#ifdef MODULE
#include "module.h"
#endif

/* offsetof is not provided in freestanding mode with -nostdinc */
#ifndef offsetof
#define offsetof(type, member)  __builtin_offsetof(type, member)
#endif

/* ── Forward declarations of PCI helpers (static within this file) ── */

/*
 * Walk the PCI capability list and return the offset of a virtio
 * vendor-specific capability (ID 0x09) of the given cfg_type.
 * Returns the capability offset in config space, or 0 if not found.
 */
static uint8_t pci_find_virtio_cap(uint8_t bus, uint8_t slot, uint8_t func,
                                   uint8_t cfg_type)
{
	uint32_t status = pci_read(bus, slot, func, 0x06);
	if (!(status & (1U << 4)))
		return 0; /* capabilities list not present */

	uint32_t caps_ptr_raw = pci_read(bus, slot, func, 0x34);
	uint8_t cap_ptr = (uint8_t)(caps_ptr_raw & 0xFF);
	int iter = 0;

	while (cap_ptr != 0) {
		if (++iter > 64)
			break;
		uint32_t cap_reg = pci_read(bus, slot, func, cap_ptr);
		uint8_t cap_id = (uint8_t)(cap_reg & 0xFF);

		if (cap_id == VIRTIO_PCI_VENDOR_CAP_ID) {
			uint8_t this_type = (uint8_t)((cap_reg >> 24) & 0xFF);
			if (this_type == cfg_type)
				return cap_ptr;
		}

		cap_ptr = (uint8_t)((cap_reg >> 8) & 0xFF);
	}

	return 0;
}

/*
 * Read the full virtio_pci_cap structure from PCI config space
 * at the given capability offset.
 */
static void pci_read_virtio_cap(uint8_t bus, uint8_t slot, uint8_t func,
                                uint8_t cap_offset,
                                struct virtio_pci_cap *out)
{
	if (!out)
		return;

	/* First dword: vndr, next, len, cfg_type */
	uint32_t dw = pci_read(bus, slot, func, cap_offset);
	out->cap_vndr = (uint8_t)(dw & 0xFF);
	out->cap_next = (uint8_t)((dw >> 8) & 0xFF);
	out->cap_len  = (uint8_t)((dw >> 16) & 0xFF);
	out->cfg_type = (uint8_t)((dw >> 24) & 0xFF);

	/* Second dword: bar, padding[3] */
	dw = pci_read(bus, slot, func, cap_offset + 4);
	out->bar = (uint8_t)(dw & 0xFF);
	out->padding[0] = (uint8_t)((dw >> 8) & 0xFF);
	out->padding[1] = (uint8_t)((dw >> 16) & 0xFF);
	out->padding[2] = (uint8_t)((dw >> 24) & 0xFF);

	/* Third dword: offset */
	out->offset = pci_read(bus, slot, func, cap_offset + 8);

	/* Fourth dword: length */
	out->length = pci_read(bus, slot, func, cap_offset + 12);
}

/* ── API implementation ──────────────────────────────────────────── */

int virtio_pci_modern_probe(struct pci_device *dev,
                            struct vpci_modern_device *vdev)
{
	uint8_t bus, slot, func;
	struct virtio_pci_cap vcap;
	uint8_t cap_off;
	int found_any = 0;

	if (!dev || !vdev)
		return -1;

	memset(vdev, 0, sizeof(*vdev));
	vdev->pci_dev = dev;

	bus  = dev->bus;
	slot = dev->slot;
	func = dev->func;

	/*
	 * Check for PCI revision >= 1.  Legacy virtio devices have rev 0;
	 * transitional and modern devices have rev >= 1.
	 */
	uint32_t rev_reg = pci_read(bus, slot, func, 0x08);
	uint8_t revision = (uint8_t)((rev_reg >> 8) & 0xFF);
	if (revision < 1) {
		/* Legacy device — not modern */
		vdev->modern_found = 0;
		return -1;
	}

	/* ── Common cfg cap (type 1) ─────────────────────────────── */
	cap_off = pci_find_virtio_cap(bus, slot, func,
	                              VIRTIO_PCI_CAP_COMMON_CFG);
	if (cap_off) {
		pci_read_virtio_cap(bus, slot, func, cap_off, &vcap);
		/*
		 * Map using PHYS_TO_VIRT on the BAR physical address.
		 * BAR stores the physical base address; we add the
		 * capability offset within the BAR.
		 */
		uint64_t bar_base = (uint64_t)(dev->bar[vcap.bar] & ~0xFu);
		if (bar_base) {
			vdev->caps.common = (volatile struct virtio_pci_common_cfg *)
				((uintptr_t)PHYS_TO_VIRT(bar_base + vcap.offset));
			found_any = 1;
		}
	}

	/* ── Notify cap (type 2) ─────────────────────────────────── */
	cap_off = pci_find_virtio_cap(bus, slot, func,
	                              VIRTIO_PCI_CAP_NOTIFY_CFG);
	if (cap_off) {
		uint32_t notify_dw[5];
		/*
		 * Read the base cap (16 bytes) + notify_off_multiplier (4 bytes)
		 * as five consecutive 32-bit words.
		 */
		for (int i = 0; i < 5; i++)
			notify_dw[i] = pci_read(bus, slot, func,
			                          cap_off + i * 4);

		uint8_t notify_bar = (uint8_t)(notify_dw[1] & 0xFF);
		uint32_t notify_bar_off = notify_dw[2];
		uint32_t notify_len = notify_dw[3];
		uint32_t multiplier = notify_dw[4];

		uint64_t bar_base = (uint64_t)(dev->bar[notify_bar] & ~0xFu);
		if (bar_base && notify_len > 0) {
			vdev->caps.notify_base = (volatile void *)
				((uintptr_t)PHYS_TO_VIRT(bar_base + notify_bar_off));
			vdev->caps.notify_off_multiplier = multiplier;
			vdev->caps.notify_base_off = notify_bar_off;
			found_any = 1;
		}
	}

	/* ── ISR cap (type 3) ────────────────────────────────────── */
	cap_off = pci_find_virtio_cap(bus, slot, func,
	                              VIRTIO_PCI_CAP_ISR_CFG);
	if (cap_off) {
		pci_read_virtio_cap(bus, slot, func, cap_off, &vcap);
		uint64_t bar_base = (uint64_t)(dev->bar[vcap.bar] & ~0xFu);
		if (bar_base) {
			vdev->caps.isr = (volatile uint8_t *)
				((uintptr_t)PHYS_TO_VIRT(bar_base + vcap.offset));
			found_any = 1;
		}
	}

	/* ── Device cfg cap (type 4) ─────────────────────────────── */
	cap_off = pci_find_virtio_cap(bus, slot, func,
	                              VIRTIO_PCI_CAP_DEVICE_CFG);
	if (cap_off) {
		pci_read_virtio_cap(bus, slot, func, cap_off, &vcap);
		uint64_t bar_base = (uint64_t)(dev->bar[vcap.bar] & ~0xFu);
		if (bar_base) {
			vdev->caps.device_cfg = (volatile void *)
				((uintptr_t)PHYS_TO_VIRT(bar_base + vcap.offset));
			found_any = 1;
		}
	}

	vdev->modern_found = found_any ? 1 : 0;
	return found_any ? 0 : -1;
}

int virtio_pci_modern_map_bars(struct vpci_modern_device *vdev)
{
	if (!vdev || !vdev->modern_found)
		return -1;

	/*
	 * PHYS_TO_VIRT was applied during probe() based on the BAR
	 * physical address read via dev->bar[].  The kernel identity
	 * mapping (KERNEL_VMA_OFFSET) makes all physical memory
	 * accessible at virtual address PHYS_TO_VIRT(phys).
	 *
	 * No additional mapping is needed — the kernel's direct map
	 * already covers all physical addresses.  On some architectures
	 * we might need to set uncacheable flags, but for x86-64 the
	 * MTRR / PAT system handles this via the BAR's memory type.
	 *
	 * This function exists as a hook for future platforms that
	 * require explicit MMU mapping of device memory.
	 */
	return 0;
}

/*
 * Write a 32-bit value to the common cfg via MMIO, handling the
 * volatile pointer correctly.
 */
static inline void vpci_cfg_writel(struct vpci_modern_device *vdev,
                                   size_t offset, uint32_t val)
{
	volatile uint32_t *ptr = (volatile uint32_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	*ptr = val;
}

static inline uint32_t vpci_cfg_readl(struct vpci_modern_device *vdev,
                                       size_t offset)
{
	volatile uint32_t *ptr = (volatile uint32_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	return *ptr;
}

static inline void vpci_cfg_writew(struct vpci_modern_device *vdev,
                                    size_t offset, uint16_t val)
{
	volatile uint16_t *ptr = (volatile uint16_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	*ptr = val;
}

static inline uint16_t vpci_cfg_readw(struct vpci_modern_device *vdev,
                                       size_t offset)
{
	volatile uint16_t *ptr = (volatile uint16_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	return *ptr;
}

static inline void vpci_cfg_writeb(struct vpci_modern_device *vdev,
                                    size_t offset, uint8_t val)
{
	volatile uint8_t *ptr = (volatile uint8_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	*ptr = val;
}

static inline uint8_t vpci_cfg_readb(struct vpci_modern_device *vdev,
                                      size_t offset)
{
	volatile uint8_t *ptr = (volatile uint8_t *)
		((volatile uint8_t *)vdev->caps.common + offset);
	return *ptr;
}

/*
 * Helper: read Device feature bits (64-bit via select register).
 */
static uint64_t virtio_pci_modern_get_device_features(
	struct vpci_modern_device *vdev)
{
	if (!vdev || !vdev->caps.common)
		return 0;

	/* Select low 32 bits */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_feature_select), 0);
	uint64_t low = vpci_cfg_readl(vdev, offsetof(struct virtio_pci_common_cfg,
	                                               device_feature));

	/* Select high 32 bits */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_feature_select), 1);
	uint64_t high = vpci_cfg_readl(vdev, offsetof(struct virtio_pci_common_cfg,
	                                                device_feature));

	return low | (high << 32);
}

/*
 * Helper: Write Driver feature bits (64-bit via select register).
 */
static void virtio_pci_modern_set_driver_features(
	struct vpci_modern_device *vdev, uint64_t features)
{
	if (!vdev || !vdev->caps.common)
		return;

	/* Select low 32 bits */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                driver_feature_select), 0);
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                driver_feature),
	                (uint32_t)(features & 0xFFFFFFFFu));

	/* Select high 32 bits */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                driver_feature_select), 1);
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                driver_feature),
	                (uint32_t)(features >> 32));
}

int virtio_pci_modern_init_device(struct vpci_modern_device *vdev,
                                  uint32_t supported, uint32_t required,
                                  const struct virtio_feature_entry *feat_table,
                                  const char *driver_name)
{
	char buf[256];

	if (!vdev || !vdev->caps.common)
		return -1;

	if (!driver_name)
		driver_name = "virtio-modern";

	/* Step 1: Reset the device */
	vpci_cfg_writeb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_status), 0);
	__asm__ volatile("" ::: "memory");

	/* Wait for reset completion (poll until status is 0) */
	uint32_t timeout = 100000;
	while (vpci_cfg_readb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                      device_status)) != 0 && timeout--)
		__asm__ volatile("pause");

	if (timeout == 0) {
		kprintf("%s: timeout waiting for device reset\n", driver_name);
		return -1;
	}

	/* Step 2: Set ACKNOWLEDGE status */
	vpci_cfg_writeb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_status),
	                VIRTIO_STATUS_ACKNOWLEDGE);
	__asm__ volatile("" ::: "memory");

	/* Step 3: Set DRIVER status */
	uint8_t st = vpci_cfg_readb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                             device_status));
	st |= VIRTIO_STATUS_DRIVER;
	vpci_cfg_writeb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_status), st);
	__asm__ volatile("" ::: "memory");

	/* Step 4: Read device features (64-bit) */
	uint64_t host_feat = virtio_pci_modern_get_device_features(vdev);

	/* Log host features */
	buf[0] = '\0';
	virtio_format_features(buf, sizeof(buf), (uint32_t)host_feat,
	                       feat_table, virtio_common_features);
	kprintf("%s: device features (0x%08X_%08X): %s\n", driver_name,
	        (unsigned int)(host_feat >> 32), (unsigned int)(host_feat & 0xFFFFFFFFu),
	        buf[0] ? buf : "(none)");

	/*
	 * Step 5: Negotiate features.
	 * For the modern transport, we use only the lower 32 bits for
	 * feature negotiation (virtio 1.0 uses 32-bit feature space
	 * within the device-specific sub-features; the upper 32 bits
	 * are reserved for future use in many implementations).
	 */
	uint32_t host_lo = (uint32_t)(host_feat & 0xFFFFFFFFu);

	/* Validate required features */
	if ((host_lo & required) != required) {
		uint32_t missing = required & ~host_lo;
		buf[0] = '\0';
		virtio_format_features(buf, sizeof(buf), missing,
		                       feat_table, virtio_common_features);
		kprintf("%s: ERROR: device missing required features (0x%08X): %s\n",
		        driver_name, (unsigned int)missing,
		        buf[0] ? buf : "(unknown)");
		return -1;
	}

	uint32_t guest_feat = host_lo & supported;

	/* Write negotiated features */
	virtio_pci_modern_set_driver_features(vdev, (uint64_t)guest_feat);
	__asm__ volatile("" ::: "memory");

	/* Step 6: Set FEATURES_OK */
	st = vpci_cfg_readb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                     device_status));
	st |= VIRTIO_STATUS_FEATURES_OK;
	vpci_cfg_writeb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_status), st);
	__asm__ volatile("" ::: "memory");

	/* Step 7: Verify device accepted the features */
	st = vpci_cfg_readb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                     device_status));
	if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
		kprintf("%s: ERROR: device rejected negotiated features (0x%08X, status=0x%02X)\n",
		        driver_name, (unsigned int)guest_feat, (unsigned int)st);
		return -1;
	}

	/* Step 8: Check feature dependencies */
	{
		char dep_err[128];
		if (virtio_check_dependencies(guest_feat, dep_err,
		                               sizeof(dep_err)) < 0) {
			kprintf("%s: WARNING: %s — continuing (device may misbehave)\n",
			        driver_name, dep_err);
		}
	}

	/* Log negotiated features */
	buf[0] = '\0';
	virtio_format_features(buf, sizeof(buf), guest_feat,
	                       feat_table, virtio_common_features);
	kprintf("%s: negotiated features (0x%08X): %s\n", driver_name,
	        (unsigned int)guest_feat, buf[0] ? buf : "(none)");

	/*
	 * Step 9: Set DRIVER_OK — device is now live.
	 * The caller is responsible for setting up virtqueues and
	 * MSI-X before calling this function, or we do it here and
	 * the driver sets up queues during its probe.
	 */
	st = vpci_cfg_readb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                     device_status));
	st |= VIRTIO_STATUS_DRIVER_OK;
	vpci_cfg_writeb(vdev, offsetof(struct virtio_pci_common_cfg,
	                                device_status), st);
	__asm__ volatile("" ::: "memory");

	return 0;
}

int virtio_pci_modern_setup_queue(struct vpci_modern_device *vdev,
                                  uint16_t queue_idx, uint16_t queue_size,
                                  uint64_t desc_paddr, uint64_t driver_paddr,
                                  uint64_t device_paddr)
{
	if (!vdev || !vdev->caps.common)
		return -1;

	/* Select the queue */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_select), queue_idx);
	__asm__ volatile("" ::: "memory");

	/*
	 * Check the maximum queue size the device supports.
	 * If the device advertises a smaller size, use that.
	 */
	uint16_t dev_max_size = vpci_cfg_readw(vdev,
		offsetof(struct virtio_pci_common_cfg, queue_size));
	if (dev_max_size == 0) {
		kprintf("[VPCI-MODERN] queue %u: device reports size 0\n",
		        (unsigned int)queue_idx);
		return -1;
	}
	if (queue_size > dev_max_size)
		queue_size = dev_max_size;

	/* Write the queue size */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_size), queue_size);
	__asm__ volatile("" ::: "memory");

	/*
	 * Write the physical addresses of the three virtqueue areas.
	 * For the modern transport, these are full 64-bit physical
	 * addresses (not PFN-shifted like legacy).
	 */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_desc),
	                (uint32_t)(desc_paddr & 0xFFFFFFFFu));
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_desc) + 4,
	                (uint32_t)(desc_paddr >> 32));

	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_driver),
	                (uint32_t)(driver_paddr & 0xFFFFFFFFu));
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_driver) + 4,
	                (uint32_t)(driver_paddr >> 32));

	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_device),
	                (uint32_t)(device_paddr & 0xFFFFFFFFu));
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_device) + 4,
	                (uint32_t)(device_paddr >> 32));
	__asm__ volatile("" ::: "memory");

	/* Enable the queue */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_enable), 1);
	__asm__ volatile("" ::: "memory");

	/* Verify queue was enabled */
	uint16_t enabled = vpci_cfg_readw(vdev,
		offsetof(struct virtio_pci_common_cfg, queue_enable));
	if (!enabled) {
		kprintf("[VPCI-MODERN] queue %u: failed to enable\n",
		        (unsigned int)queue_idx);
		return -1;
	}

	return 0;
}

int virtio_pci_modern_setup_packed_queue(struct vpci_modern_device *vdev,
                                         uint16_t queue_idx, uint16_t queue_size,
                                         uint64_t desc_paddr)
{
	if (!vdev || !vdev->caps.common)
		return -1;

	/* Select the queue */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_select), queue_idx);
	__asm__ volatile("" ::: "memory");

	/*
	 * Check the maximum queue size the device supports.
	 * If the device advertises a smaller size, use that.
	 */
	uint16_t dev_max_size = vpci_cfg_readw(vdev,
		offsetof(struct virtio_pci_common_cfg, queue_size));
	if (dev_max_size == 0) {
		kprintf("[VPCI-MODERN] packed queue %u: device reports size 0\n",
		        (unsigned int)queue_idx);
		return -1;
	}
	if (queue_size > dev_max_size)
		queue_size = dev_max_size;

	/* Write the queue size */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_size), queue_size);
	__asm__ volatile("" ::: "memory");

	/*
	 * For packed virtqueues (virtio 1.1), only queue_desc is used.
	 * queue_driver and queue_device must be written as 0.
	 */
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_desc),
	                (uint32_t)(desc_paddr & 0xFFFFFFFFu));
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_desc) + 4,
	                (uint32_t)(desc_paddr >> 32));

	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_driver), 0);
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_driver) + 4, 0);

	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_device), 0);
	vpci_cfg_writel(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_device) + 4, 0);
	__asm__ volatile("" ::: "memory");

	/* Enable the queue */
	vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
	                                queue_enable), 1);
	__asm__ volatile("" ::: "memory");

	/* Verify queue was enabled */
	uint16_t enabled = vpci_cfg_readw(vdev,
		offsetof(struct virtio_pci_common_cfg, queue_enable));
	if (!enabled) {
		kprintf("[VPCI-MODERN] packed queue %u: failed to enable\n",
		        (unsigned int)queue_idx);
		return -1;
	}

	return 0;
}

void virtio_pci_modern_notify_queue(struct vpci_modern_device *vdev,
                                    uint16_t queue_idx)
{
	uint32_t notify_off;

	if (!vdev || !vdev->caps.notify_base)
		return;

	/*
	 * Calculate the notification address using the notify_off_multiplier.
	 *
	 * First we need to know the queue_notify_off for this queue:
	 * select the queue in the common cfg and read queue_notify_off.
	 */
	if (vdev->caps.common) {
		vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
		                                queue_select), queue_idx);
		__asm__ volatile("" ::: "memory");
		uint16_t qn_off = vpci_cfg_readw(vdev,
			offsetof(struct virtio_pci_common_cfg, queue_notify_off));

		notify_off = (uint32_t)qn_off * vdev->caps.notify_off_multiplier;
	} else {
		/* Fallback: assume queue index == notify offset */
		notify_off = (uint32_t)queue_idx * vdev->caps.notify_off_multiplier;
	}

	volatile uint32_t *notify_addr = (volatile uint32_t *)
		((volatile uint8_t *)vdev->caps.notify_base + notify_off);

	*notify_addr = (uint32_t)queue_idx;
	__asm__ volatile("" ::: "memory");
}

int virtio_pci_modern_enable_msix(struct vpci_modern_device *vdev,
                                  const uint8_t *vectors,
                                  const uint32_t *apic_ids, int n)
{
	struct msix_info msix_info;
	uint8_t bus, slot, func;
	int ret;

	if (!vdev || !vectors || !apic_ids || n < 1)
		return -1;

	bus  = vdev->pci_dev->bus;
	slot = vdev->pci_dev->slot;
	func = vdev->pci_dev->func;

	/* Find MSI-X capability */
	ret = pci_find_msix_cap(bus, slot, func, &msix_info);
	if (ret < 0) {
		kprintf("[VPCI-MODERN] device %02x:%02x.%d has no MSI-X capability\n",
		        (unsigned int)bus, (unsigned int)slot, (unsigned int)func);
		return -1;
	}

	/*
	 * Clamp 'n' to the device's MSI-X table size.
	 */
	if (n > (int)msix_info.table_size)
		n = (int)msix_info.table_size;

	/*
	 * Map the MSI-X table BAR into virtual address space.
	 * The table must be accessed as a set of 32-bit entries.
	 *
	 * MSI-X table entry format (per entry):
	 *   [0]  = lower message address (32-bit)
	 *   [4]  = upper message address (32-bit) — 0 for 32-bit
	 *   [8]  = message data (32-bit)
	 *   [12] = vector control (32-bit)
	 */
	uint64_t table_phys = (uint64_t)(vdev->pci_dev->bar[msix_info.table_bir] & ~0xFu);
	if (!table_phys) {
		kprintf("[VPCI-MODERN] MSI-X table BAR is I/O space?\n");
		return -1;
	}
	table_phys += msix_info.table_offset;

	volatile uint32_t *table_virt = (volatile uint32_t *)
		((uintptr_t)PHYS_TO_VIRT(table_phys));

	/*
	 * Program each MSI-X table entry with the specified vector and
	 * APIC ID.  The message address is:
	 *   lower = 0xFEE00000 | (apic_id << 12)
	 *   upper = 0
	 * Message data = vector number (delivery mode = Fixed)
	 */
	for (int i = 0; i < n; i++) {
		uint32_t entry_base = (uint32_t)i * 4; /* 4 dwords per entry */

		/* Message Address (lower) — 0xFEE00000 | (APIC_ID << 12) */
		uint32_t msg_addr_low = 0xFEE00000u | (apic_ids[i] << 12);
		table_virt[entry_base]     = msg_addr_low;

		/* Message Address (upper) — 0 for x86 32-bit MSI */
		table_virt[entry_base + 1] = 0;

		/* Message Data — vector number (edge-triggered, fixed) */
		table_virt[entry_base + 2] = (uint32_t)vectors[i];

		/* Vector Control — 0 = unmasked */
		table_virt[entry_base + 3] = 0;
	}

	/*
	 * Flush writes before enabling MSI-X.
	 */
	__asm__ volatile("" ::: "memory");

	/*
	 * Enable MSI-X in the PCI config space:
	 *   - Write Message Control register at cap+2, bit 15 = MSI-X Enable
	 *   - Also set bit 14 = Function Mask clear
	 *
	 * pci_read/pci_write operate on full 32-bit dwords, so we do
	 * read-modify-write on the word containing the message control.
	 */
	{
		uint32_t ctrl_dword = pci_read(bus, slot, func,
		                                msix_info.cap_offset + 2);
		uint32_t ctrl_new = ctrl_dword;
		if (ctrl_new & 0xFFFF0000u) {
			/* Preserve high 16 bits, modify low 16 bits */
			ctrl_new &= 0xFFFF0000u;
		} else {
			ctrl_new = 0;
		}
		ctrl_new |= (1U << 15);  /* MSI-X Enable */
		ctrl_new &= ~(1U << 14); /* Function Mask clear */
		pci_write(bus, slot, func, msix_info.cap_offset + 2,
		          ctrl_new);
	}

	/*
	 * Configure the MSI-X vector for the config change interrupt
	 * in the common cfg structure (if common cfg is available).
	 */
	if (vdev->caps.common && n > 0) {
		/* Use vector 0 for config changes */
		vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
		                                msix_config), 0);
	}

	kprintf("[VPCI-MODERN] MSI-X enabled: %d vector(s)\n", n);
	return 0;
}

void virtio_pci_modern_disable_msix(struct vpci_modern_device *vdev)
{
	struct msix_info msix_info;
	uint8_t bus, slot, func;

	if (!vdev)
		return;

	bus  = vdev->pci_dev->bus;
	slot = vdev->pci_dev->slot;
	func = vdev->pci_dev->func;

	/* First, set the config vector to NO_VECTOR (= 0xFFFF) */
	if (vdev->caps.common) {
		vpci_cfg_writew(vdev, offsetof(struct virtio_pci_common_cfg,
		                                msix_config), 0xFFFF);
	}

	/* Disable MSI-X in config space */
	if (pci_find_msix_cap(bus, slot, func, &msix_info) == 0) {
		uint32_t ctrl_dword = pci_read(bus, slot, func,
		                                msix_info.cap_offset + 2);
		uint32_t ctrl_new = ctrl_dword;
		if (ctrl_new & 0xFFFF0000u)
			ctrl_new &= 0xFFFF0000u;
		else
			ctrl_new = 0;
		ctrl_new &= ~(1U << 15); /* Clear MSI-X Enable */
		pci_write(bus, slot, func, msix_info.cap_offset + 2,
		          ctrl_new);
	}
}

/*
 * Read device-specific config from the modern MMIO region.
 */
uint32_t virtio_pci_modern_read_cfg(struct vpci_modern_device *vdev,
                                    uint32_t offset, void *buf, uint32_t len)
{
	if (!vdev || !vdev->caps.device_cfg || !buf || len == 0)
		return 0;

	volatile uint8_t *src = (volatile uint8_t *)vdev->caps.device_cfg;
	uint8_t *dst = (uint8_t *)buf;

	/*
	 * The device config region is in native-endian (little-endian
	 * on x86).  Read byte-by-byte for arbitrary alignment.
	 */
	for (uint32_t i = 0; i < len; i++)
		dst[i] = src[offset + i];

	return len;
}

/*
 * Write device-specific config to the modern MMIO region.
 */
uint32_t virtio_pci_modern_write_cfg(struct vpci_modern_device *vdev,
                                     uint32_t offset, const void *buf,
                                     uint32_t len)
{
	if (!vdev || !vdev->caps.device_cfg || !buf || len == 0)
		return 0;

	volatile uint8_t *dst = (volatile uint8_t *)vdev->caps.device_cfg;
	const uint8_t *src = (const uint8_t *)buf;

	for (uint32_t i = 0; i < len; i++)
		dst[offset + i] = src[i];

	return len;
}

/* ── Module init/exit stubs ───────────────────────────────────────── */

#ifdef MODULE
int __init init_module(void)
{
	kprintf("[VIRTIO] Modern PCI transport loaded\n");
	return 0;
}

void __exit cleanup_module(void)
{
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Modern VirtIO PCI transport (MMIO + MSI-X)");
MODULE_VERSION("1.0");
#endif
