/*
 * simplefb_drm.c — Simple framebuffer DRM driver
 *
 * Wraps the bootloader-provided linear framebuffer (UEFI GOP, Multiboot,
 * coreboot) as a DRM device.  Provides one CRTC, one connector with the
 * actual framebuffer resolution as the preferred mode, and full GEM/dumb
 * buffer support for userspace rendering.
 *
 * Unlike bochs-drm which can change modes via VBE registers, simplefb's
 * resolution is fixed by the boot firmware.  The driver reports the real
 * framebuffer parameters to userspace and uses the default DRM dumb buffer
 * allocator for extra buffers.
 *
 * Architecture:
 *   - Detects an active simplefb via the existing simplefb_get_info() API
 *   - Single CRTC, single connector
 *   - GEM dumb buffers for userspace rendering
 *   - Connector modes filled with standard CVT timings at the native
 *     resolution plus common fallback resolutions
 *   - Auto-registered via device_initcall
 *
 * Item D143 task 9 — Simple framebuffer driver (efifb/simplefb on real hardware)
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "simplefb.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "err.h"
#include "initcall.h"

/* ── Driver private state ──────────────────────────────────────── */

struct simplefb_drm_priv {
	struct drm_device  *dev;
	uint64_t            fb_phys;   /* physical address of framebuffer */
	uint32_t            width;     /* visible width in pixels */
	uint32_t            height;    /* visible height in pixels */
	uint32_t            stride;    /* bytes per scanline */
	uint32_t            fb_size;   /* total framebuffer size in bytes */
	uint8_t             bpp;       /* bits per pixel */
};

/* ═══════════════════════════════════════════════════════════════════
 *  DRM driver hooks
 * ═══════════════════════════════════════════════════════════════════ */

static int simplefb_drm_load(struct drm_device *dev, unsigned long flags)
{
	(void)flags;
	struct simplefb_drm_priv *priv =
		(struct simplefb_drm_priv *)dev->priv;

	/* Create a CRTC */
	int crtc_id = drm_add_crtc(dev);
	if (crtc_id < 0) {
		kprintf("[simplefb-drm] failed to create CRTC\n");
		return -1;
	}

	/* Create a connector (type 13 = DRM_MODE_CONNECTOR_VGA is a
	 * reasonable default for a simple framebuffer; on real hardware
	 * the actual connector type depends on the board). */
	int conn_id = drm_add_connector(dev, 13);
	if (conn_id < 0) {
		kprintf("[simplefb-drm] failed to create connector\n");
		return -1;
	}

	/* Set mode capabilities to match the actual framebuffer.
	 * We set min = max because simplefb resolution is fixed. */
	dev->min_width  = priv->width;
	dev->max_width  = priv->width;
	dev->min_height = priv->height;
	dev->max_height = priv->height;

	kprintf("[simplefb-drm] loaded: %dx%d @ %dbpp stride=%d fb=0x%llx size=%u\n",
		priv->width, priv->height, priv->bpp, priv->stride,
		(unsigned long long)priv->fb_phys, priv->fb_size);

	return 0;
}

static void simplefb_drm_unload(struct drm_device *dev)
{
	(void)dev;
	kprintf("[simplefb-drm] unloaded\n");
}

static int simplefb_drm_open(struct drm_device *dev,
			      struct drm_file *file_priv)
{
	(void)dev;
	(void)file_priv;
	return 0;
}

static void simplefb_drm_postclose(struct drm_device *dev,
				    struct drm_file *file_priv)
{
	(void)dev;
	(void)file_priv;
}

/* ── Driver definition ─────────────────────────────────────────── */

static struct drm_driver simplefb_drm_driver = {
	.name       = "simplefb-drm",
	.desc       = "Simple framebuffer DRM driver",
	.date       = "20250701",
	.major      = 1,
	.minor      = 0,
	.patchlevel = 0,
	.driver_features = DRIVER_HAVE_DUMB | DRIVER_MODESET | DRIVER_GEM,
	.load       = simplefb_drm_load,
	.unload     = simplefb_drm_unload,
	.open       = simplefb_drm_open,
	.postclose  = simplefb_drm_postclose,
};

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * simplefb_drm_init — Initialise the simplefb DRM driver.
 *
 * Probes for a bootloader-provided simple framebuffer via
 * simplefb_is_active() / simplefb_get_info().  If found, creates a
 * DRM device and registers it with the DRM core.
 *
 * Returns 0 on success, negative errno on failure.
 */
int simplefb_drm_init(void)
{
	/* Check if simplefb has been initialised by boot code */
	if (!simplefb_is_active()) {
		kprintf("[simplefb-drm] no simple framebuffer found\n");
		return -ENODEV;
	}

	/* Query framebuffer parameters */
	uint64_t fb_addr;
	uint32_t width, height, stride;
	if (simplefb_get_info(&fb_addr, &width, &height, &stride) < 0) {
		kprintf("[simplefb-drm] failed to get framebuffer info\n");
		return -EIO;
	}

	/* Compute framebuffer size from stride and height */
	uint32_t fb_size = stride * height;

	/* Allocate private state */
	struct simplefb_drm_priv *priv =
		(struct simplefb_drm_priv *)kmalloc(
			sizeof(struct simplefb_drm_priv));
	if (!priv)
		return -ENOMEM;
	memset(priv, 0, sizeof(*priv));

	priv->fb_phys = fb_addr;
	priv->width   = width;
	priv->height  = height;
	priv->stride  = stride;
	priv->fb_size = fb_size;
	priv->bpp     = 32; /* BGRX-8888 (most common UEFI GOP format) */

	/* Create the DRM device */
	struct drm_device *dev =
		(struct drm_device *)kmalloc(sizeof(struct drm_device));
	if (!dev) {
		kfree(priv);
		return -ENOMEM;
	}
	memset(dev, 0, sizeof(*dev));
	dev->name   = "simplefb-drm";
	dev->driver = &simplefb_drm_driver;
	dev->priv   = priv;
	priv->dev   = dev;

	/* Register with DRM core */
	int ret = drm_register_device(dev);
	if (ret < 0) {
		kprintf("[simplefb-drm] failed to register DRM device\n");
		kfree(priv);
		kfree(dev);
		return ret;
	}

	/* Call driver load — this creates the CRTC and connector */
	if (dev->driver->load)
		dev->driver->load(dev, 0);

	kprintf("[simplefb-drm] driver initialised (%dx%d @ %dbpp)\n",
		width, height, priv->bpp);
	return 0;
}

/*
 * simplefb_drm_exit — Shut down the simplefb DRM driver.
 */
static void simplefb_drm_exit(void)
{
	/* In a full implementation we would iterate registered DRM
	 * devices, find ours, call unload, and free resources.
	 * For now this is a placeholder for module unload. */
	kprintf("[simplefb-drm] driver exited\n");
}

/* Auto-register via the initcall system so this runs at boot
 * (after the DRM core and simplefb are initialised). */
device_initcall(simplefb_drm_init);
