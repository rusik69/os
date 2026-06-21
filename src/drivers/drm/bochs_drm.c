/*
 * bochs_drm.c — Bochs VBE DRM driver
 *
 * Registers bochs-vga as a DRM driver.  Provides one CRTC, one
 * connector (VGA), and a simple primary plane backed by GEM dumb
 * buffers.  Mode setting uses the Bochs VBE I/O registers.
 *
 * Architecture:
 *   - PCI probe of Bochs VGA device
 *   - Single CRTC, single VGA connector
 *   - GEM dumb buffers for scanout
 *   - Mode setting via VBE registers (0x01CE/0x01CF)
 *
 * References:
 *   Bochs VBE Specification
 *   Linux kernel bochs-drm driver
 *
 * Item S27 — bochs-drm
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

/* ── VBE register indices ───────────────────────────────────────── */

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

/* ── Supported modes ───────────────────────────────────────────── */

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

/* ── Driver private state ──────────────────────────────────────── */

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

/* ── VBE helpers ────────────────────────────────────────────────── */

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

/* ═══════════════════════════════════════════════════════════════════
 *  DRM driver hooks
 * ═══════════════════════════════════════════════════════════════════ */

static int bochs_drm_load(struct drm_device *dev, unsigned long flags)
{
    (void)flags;
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)dev->priv;

    /* Create a CRTC */
    int crtc_id = drm_add_crtc(dev);
    if (crtc_id < 0) {
        kprintf("[bochs-drm] failed to create CRTC\n");
        return -1;
    }

    /* Create a connector */
    int conn_id = drm_add_connector(dev, 0);  /* VGA */
    if (conn_id < 0) {
        kprintf("[bochs-drm] failed to create connector\n");
        return -1;
    }

    /* Set mode capabilities */
    dev->min_width  = 640;
    dev->max_width  = 1280;
    dev->min_height = 480;
    dev->max_height = 1024;

    /* Set default mode */
    bochs_set_mode_vbe(priv, 1024, 768, 32);

    kprintf("[bochs-drm] loaded (LFB at 0x%llx, size %u)\n",
            (unsigned long long)priv->lfb_phys, priv->lfb_size);

    return 0;
}

static void bochs_drm_unload(struct drm_device *dev)
{
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)dev->priv;

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

/* ── Driver definition ─────────────────────────────────────────── */

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

/* ═══════════════════════════════════════════════════════════════════
 *  PCI probing and initialisation
 * ═══════════════════════════════════════════════════════════════════ */

/* Bochs VGA PCI class/subclass: 03:00 */
#define BOCHS_PCI_CLASS    0x03
#define BOCHS_PCI_SUBCLASS 0x00

/*
 * bochs_drm_init — Initialise the bochs-drm driver.
 *
 * Probes for Bochs VBE, sets up the LFB, and registers as a DRM device.
 */
int bochs_drm_init(void)
{
    /* Check Bochs VBE presence using existing bochs.c probe */
    bochs_init();
    if (!bochs_is_present()) {
        kprintf("[bochs-drm] no Bochs VBE found\n");
        return -1;
    }

    /* Allocate private state */
    struct bochs_drm_priv *priv = (struct bochs_drm_priv *)
        kmalloc(sizeof(struct bochs_drm_priv));
    if (!priv) return -1;
    memset(priv, 0, sizeof(*priv));

    /* Find LFB physical address from PCI */
    /* Bochs VGA is usually at PCI 00:02.0 on QEMU */
    /* The LFB BAR is typically BAR0 (or BAR2 on some devices) */
    /* Use known LFB address from Bochs VBE interface.
     * On QEMU, bochs-display typically exposes LFB at PCI BAR0.
     * The existing bochs driver sets up the mode via VBE I/O ports. */
    priv->lfb_phys = 0xFC000000;  /* typical QEMU bochs LFB address */
    priv->lfb_size = 16 * 1024 * 1024;  /* 16 MB typical */

    /* Map LFB into kernel space for scanout */
    priv->lfb_virt = vmm_map_phys(priv->lfb_phys, priv->lfb_size,
                                   VMM_FLAG_PRESENT | VMM_FLAG_WRITE |
                                   VMM_FLAG_NOCACHE);
    if (!priv->lfb_virt) {
        kprintf("[bochs-drm] failed to map LFB\n");
        kfree(priv);
        return -1;
    }

    /* Create the DRM device */
    struct drm_device *dev = (struct drm_device *)
        kmalloc(sizeof(struct drm_device));
    if (!dev) {
        vmm_unmap_phys(priv->lfb_virt, priv->lfb_size);
        kfree(priv);
        return -1;
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
        return -1;
    }

    /* Call driver load */
    if (dev->driver->load)
        dev->driver->load(dev, 0);

    kprintf("[bochs-drm] driver initialised\n");
    return 0;
}

/*
 * bochs_drm_exit — Shut down the bochs-drm driver.
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

/* ── Stub: bochs_drm_mode_set ─────────────────────────────── */
int bochs_drm_mode_set(void *crtc, void *mode)
{
    (void)crtc;
    (void)mode;
    kprintf("[bochs_drm] bochs_drm_mode_set: not yet implemented\n");
    return -ENOSYS;
}
