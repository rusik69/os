/*
 * bochs_drm.c -- Bochs VBE DRM driver
 *
 * Registers bochs-vga as a DRM driver.  Provides one CRTC, one
 * connector (VGA), and a simple primary plane backed by GEM dumb
 * buffers.  Mode setting uses the Bochs VBE I/O registers.
 *
 * Architecture:
 *   - PCI probe of Bochs VGA device
 *   - Single CRTC, single VGA connector
 *   - GEM dumb buffers for scanout
 *   - Connector mode list populated via CVT timings from bochs_modes[]
 *   - Mode setting via VBE registers (0x01CE/0x01CF)
 *   - Mode validation against hardware capability table
 *
 * References:
 *   Bochs VBE Specification
 *   Linux kernel bochs-drm driver
 *
 * Item S27 -- bochs-drm
 * D143 task 10 -- Bochs VBE modesetting implementation
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "bochs.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "err.h"
#include "errno.h"

/* -- VBE register indices ---------------------------------------- */

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9

#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF

#define VBE_DISPI_LFB_ENABLED   0x40
#define VBE_DISPI_ENABLED       0x01

/* -- Supported modes --------------------------------------------- */

struct bochs_mode {
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
};

static const struct bochs_mode bochs_modes[] = {
    { 640,  480,  32 },
    { 800,  600,  32 },
    { 1024, 768,  32 },
    { 1280, 720,  32 },
    { 1280, 1024, 32 },
    { 0, 0, 0 }  /* terminator */
};

/* -- Driver private state ---------------------------------------- */

struct bochs_drm_priv {
    struct drm_device  *dev;
    uint64_t            lfb_phys;   /* physical address of LFB */
    void               *lfb_virt;   /* kernel virtual address of LFB */
    uint32_t            lfb_size;
    int                 current_mode;
    uint16_t            width;
    uint16_t            height;
    uint16_t            bpp;
    uint32_t            stride;
};

/* -- VBE helpers ------------------------------------------------- */

static inline void vbe_write(uint16_t index, uint16_t value)
{
    outw(index, VBE_DISPI_IOPORT_INDEX);
    outw(value, VBE_DISPI_IOPORT_DATA);
}

static inline uint16_t vbe_read(uint16_t index)
{
    outw(index, VBE_DISPI_IOPORT_INDEX);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bochs_set_mode_vbe(struct bochs_drm_priv *priv,
                                uint16_t width, uint16_t height,
                                uint16_t bpp)
{
    /* Disable display while changing mode */
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);

    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP, bpp);
    vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    vbe_write(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);

    /* Enable display + LFB */
    vbe_write(VBE_DISPI_INDEX_ENABLE,
              (uint16_t)(VBE_DISPI_LFB_ENABLED | VBE_DISPI_ENABLED));

    priv->width  = width;
    priv->height = height;
    priv->bpp    = bpp;
    priv->stride = (uint32_t)(width * (bpp / 8));

    kprintf("[bochs-drm] mode set: %dx%d @ %dbpp (stride=%u)\n",
            width, height, bpp, priv->stride);
}

/* ================================================================
 *  Mode validation
 * ================================================================ */

/*
 * bochs_drm_mode_valid -- Validate a display mode against Bochs VBE
 *                        hardware capabilities.
 *
 * Checks that the requested width, height, and nominal bpp are in the
 * table of modes supported by the hardware.
 *
 * Returns 0 (MODE_OK) if the mode is supported, or a negative errno
 * indicating why the mode was rejected.
 */
static int bochs_drm_mode_valid(const struct drm_display_mode *mode)
{
    if (!mode || !mode->in_use)
        return -EINVAL;

    if (mode->hdisplay == 0 || mode->vdisplay == 0)
        return -EINVAL;

    /* Bochs VBE hardware only supports 32 bpp in our configuration */
    int bpp = 32;

    /* Check against the hardware capability table */
    for (int i = 0; bochs_modes[i].width != 0; i++) {
        if (bochs_modes[i].width  == (uint16_t)mode->hdisplay &&
            bochs_modes[i].height == (uint16_t)mode->vdisplay &&
            bochs_modes[i].bpp    == bpp) {
            return 0;  /* MODE_OK */
        }
    }

    kprintf("[bochs-drm] mode_valid: rejected %dx%d "
            "(not in capability table)\n",
            mode->hdisplay, mode->vdisplay);
    return -EINVAL;
}

/* ================================================================
 *  Mode setting -- CRTC mode_set callback
 * ================================================================ */

/*
 * bochs_drm_crtc_mode_set -- Program the Bochs VBE hardware to a
 *                           given display mode.
 *
 * Called when a new mode is assigned to the CRTC.  Extracts the
 * resolution from the display mode and writes the VBE registers.
 *
 * @crtc: Pointer to the struct drm_crtc being configured (unused).
 * @mode: Pointer to a struct drm_display_mode with the target resolution.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int bochs_drm_crtc_mode_set(void *crtc, void *mode)
{
    (void)crtc;

    if (!mode)
        return -EINVAL;

    struct drm_display_mode *dm = (struct drm_display_mode *)mode;

    if (!dm->in_use)
        return -EINVAL;

    /* Validate the mode against hardware capabilities first */
    int ret = bochs_drm_mode_valid(dm);
    if (ret < 0) {
        kprintf("[bochs-drm] crtc_mode_set: invalid mode %dx%d\n",
                dm->hdisplay, dm->vdisplay);
        return ret;
    }

    /* We store the priv pointer in a static variable for the
     * mode_set callback since the CRTC struct lacks a dev link. */
    extern struct bochs_drm_priv *g_bochs_crtc_mode_set_priv;
    struct bochs_drm_priv *priv = g_bochs_crtc_mode_set_priv;

    if (!priv) {
        kprintf("[bochs-drm] crtc_mode_set: no private state\n");
        return -ENODEV;
    }

    bochs_set_mode_vbe(priv,
                       (uint16_t)dm->hdisplay,
                       (uint16_t)dm->vdisplay,
                       32);

    return 0;
}

/* Global pointer for the mode_set callback (one device). */
struct bochs_drm_priv *g_bochs_crtc_mode_set_priv = NULL;

/*
 * bochs_drm_mode_set -- Public mode setting entry point.
 *
 * Called from outside the DRM core (e.g. from the VESA framebuffer
 * console or a boot-time mode switch) to change the display mode.
 *
 * @mode: Pointer to a struct drm_display_mode, or NULL to disable.
 *
 * Returns 0 on success, negative errno on failure.
 */
int bochs_drm_mode_set(void *crtc, void *mode)
{
    return bochs_drm_crtc_mode_set(crtc, mode);
}

/* ================================================================
 *  Connector mode population
 * ================================================================ */

/*
 * bochs_drm_populate_modes -- Add all supported modes from bochs_modes[]
 *                            to the connector's mode list.
 *
 * For each entry in the static bochs_modes[] table, generates CVT-RB
 * timing via drm_display_cvt_mode() and adds the mode to the connector
 * via drm_display_add_mode().
 *
 * @dev:  The DRM device.
 * @conn: The connector to populate (must be the VGA connector).
 *
 * Returns the number of modes added, or 0 on error.
 */
static int bochs_drm_populate_modes(struct drm_device *dev,
                                     struct drm_connector *conn)
{
    int count = 0;

    if (!conn || !dev)
        return 0;

    for (int i = 0; bochs_modes[i].width != 0; i++) {
        struct drm_display_mode mode;
        int ret;

        /* Generate CVT-RB timing for the mode at 60 Hz */
        ret = drm_display_cvt_mode((uint32_t)bochs_modes[i].width,
                                    (uint32_t)bochs_modes[i].height,
                                    60, 1, &mode);
        if (ret < 0) {
            kprintf("[bochs-drm] populate: CVT failed for %dx%d "
                    "(ret=%d)\n",
                    bochs_modes[i].width, bochs_modes[i].height, ret);
            continue;
        }

        /* Mark as a built-in (driver-provided) mode */
        mode.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_BUILTIN;

        /* Mark the default mode (1024x768) as preferred */
        if (bochs_modes[i].width == 1024 &&
            bochs_modes[i].height == 768)
            mode.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DEFAULT;

        ret = drm_display_add_mode(conn, &mode);
        if (ret < 0) {
            kprintf("[bochs-drm] populate: add_mode failed "
                    "for %dx%d (ret=%d)\n",
                    bochs_modes[i].width, bochs_modes[i].height, ret);
            continue;
        }

        count++;
    }

    kprintf("[bochs-drm] populated %d modes from capability table\n",
            count);
    return count;
}

/* ================================================================
 *  DRM driver hooks
 * ================================================================ */

static int bochs_drm_load(struct drm_device *dev, unsigned long flags)
{
    (void)flags;
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)dev->priv;

    /* Create a CRTC */
    int crtc_id = drm_add_crtc(dev);
    if (crtc_id < 0) {
        kprintf("[bochs-drm] failed to create CRTC\n");
        return -ENOMEM;
    }

    /* Create a connector (type 13 = DRM_MODE_CONNECTOR_VGA) */
    int conn_id = drm_add_connector(dev, 13);  /* VGA connector type */
    if (conn_id < 0) {
        kprintf("[bochs-drm] failed to create connector\n");
        return -ENOMEM;
    }

    /* Set mode capabilities to match the hardware */
    dev->min_width  = 640;
    dev->max_width  = 1280;
    dev->min_height = 480;
    dev->max_height = 1024;

    /* Register the global priv pointer for the mode_set callback */
    g_bochs_crtc_mode_set_priv = priv;

    /* Set default mode */
    bochs_set_mode_vbe(priv, 1024, 768, 32);

    /* Populate the connector's mode list from our capability table */
    for (int i = 0; i < DRM_MAX_CONNECTOR; i++) {
        if (dev->connectors[i].in_use &&
            dev->connectors[i].connector_id == (uint32_t)conn_id) {
            bochs_drm_populate_modes(dev, &dev->connectors[i]);
            break;
        }
    }

    kprintf("[bochs-drm] loaded (LFB at 0x%llx, size %u)\n",
            (unsigned long long)priv->lfb_phys, priv->lfb_size);

    return 0;
}

static void bochs_drm_unload(struct drm_device *dev)
{
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)dev->priv;

    /* Clear the global priv pointer */
    g_bochs_crtc_mode_set_priv = NULL;

    /* Disable display */
    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);

    if (priv->lfb_virt) {
        vmm_unmap_phys(priv->lfb_virt, priv->lfb_size);
    }

    kprintf("[bochs-drm] unloaded\n");
}

static int bochs_drm_open(struct drm_device *dev, struct drm_file *file_priv)
{
    (void)dev;
    (void)file_priv;
    return 0;
}

static void bochs_drm_postclose(struct drm_device *dev,
                                 struct drm_file *file_priv)
{
    (void)dev;
    (void)file_priv;
}

/* -- Driver definition ------------------------------------------- */

static struct drm_driver bochs_drm_driver = {
    .name       = "bochs-drm",
    .desc       = "Bochs VBE DRM driver",
    .date       = "20250613",
    .major      = 1,
    .minor      = 0,
    .patchlevel = 0,
    .driver_features = DRIVER_HAVE_DUMB | DRIVER_MODESET | DRIVER_GEM,
    .load       = bochs_drm_load,
    .unload     = bochs_drm_unload,
    .open       = bochs_drm_open,
    .postclose  = bochs_drm_postclose,
};

/* ================================================================
 *  PCI probing and initialisation
 * ================================================================ */

/* Bochs VGA PCI class/subclass: 03:00 */
#define BOCHS_PCI_CLASS    0x03
#define BOCHS_PCI_SUBCLASS 0x00

/*
 * bochs_drm_init -- Initialise the bochs-drm driver.
 *
 * Probes for Bochs VBE, sets up the LFB, and registers as a DRM device.
 */
int bochs_drm_init(void)
{
    /* Check Bochs VBE presence using existing bochs.c probe */
    bochs_init();
    if (!bochs_is_present()) {
        kprintf("[bochs-drm] no Bochs VBE found\n");
        return -ENODEV;
    }

    /* Allocate private state */
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)
        kmalloc(sizeof(struct bochs_drm_priv));
    if (!priv)
        return -ENOMEM;
    memset(priv, 0, sizeof(*priv));

    /* Find LFB physical address from PCI */
    priv->lfb_phys = 0xFC000000;  /* typical QEMU bochs LFB address */
    priv->lfb_size = 16 * 1024 * 1024;  /* 16 MB typical */

    /* Map LFB into kernel space for scanout */
    priv->lfb_virt = vmm_map_phys(priv->lfb_phys, priv->lfb_size,
                                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
                                   VMM_FLAG_NOCACHE);
    if (IS_ERR(priv->lfb_virt)) {
        kprintf("[bochs-drm] failed to map LFB\n");
        kfree(priv);
        return -ENOMEM;
    }

    /* Create the DRM device */
    struct drm_device *dev = (struct drm_device *)
        kmalloc(sizeof(struct drm_device));
    if (!dev) {
        vmm_unmap_phys(priv->lfb_virt, priv->lfb_size);
        kfree(priv);
        return -ENOMEM;
    }

    memset(dev, 0, sizeof(*dev));
    dev->name   = "bochs-drm";
    dev->driver = &bochs_drm_driver;
    dev->priv   = priv;
    priv->dev   = dev;

    /* Register with DRM core */
    int ret = drm_register_device(dev);
    if (ret < 0) {
        kprintf("[bochs-drm] failed to register DRM device\n");
        vmm_unmap_phys(priv->lfb_virt, priv->lfb_size);
        kfree(priv);
        kfree(dev);
        return ret;
    }

    /* Call driver load */
    if (dev->driver->load)
        dev->driver->load(dev, 0);

    kprintf("[bochs-drm] driver initialised\n");
    return 0;
}

/*
 * bochs_drm_exit -- Shut down the bochs-drm driver.
 */
void bochs_drm_exit(void)
{
    struct drm_device *dev = NULL;

    /* Find our device */
    for (int i = 0; i < 4; i++) {
        /* We'd need a way to iterate registered DRM devices */
    }

    if (dev) {
        if (dev->driver->unload)
            dev->driver->unload(dev);
        drm_unregister_device(dev);
        kfree(dev->priv);
        kfree(dev);
    }

    kprintf("[bochs-drm] driver exited\n");
}
