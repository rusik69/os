/*
 * drm_dumb.c — DRM dumb buffer allocation
 *
 * Implements DRM_IOCTL_MODE_CREATE_DUMB, DRM_IOCTL_MODE_MAP_DUMB,
 * and DRM_IOCTL_MODE_DESTROY_DUMB.  Dumb buffers are simple
 * framebuffers allocated via GEM, used by fbdev emulation and
 * simple DRM clients.
 *
 * A dumb buffer is a GEM-backed buffer object created with a
 * simple linear layout (no tiling or compression).
 *
 * Item S25 — DRM dumb buffer
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"

/* ── Pitch calculation ─────────────────────────────────────────── */

/*
 * Calculate the pitch (bytes per row) for a dumb buffer.
 * Aligns to 64 bytes for cache-line compatibility.
 */
static uint32_t dumb_calc_pitch(uint32_t width, uint32_t bpp)
{
    uint32_t pitch = (width * bpp + 7) / 8;
    /* Align to 64 bytes */
    pitch = (pitch + 63) & ~63U;
    return pitch;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Dumb buffer operations
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_dumb_create — Create a dumb buffer.
 *
 * Allocates a GEM buffer object with size sufficient for the
 * requested resolution and bpp.  Returns the GEM handle and pitch.
 *
 * @dev:   DRM device.
 * @args:  Arguments from userspace (height, width, bpp).
 *         Returns handle, pitch, size.
 *
 * Returns 0 on success.
 */
int drm_dumb_create(struct drm_device *dev,
                     struct drm_mode_create_dumb *args)
{
    if (!dev || !args)
        return -1;

    /* Validate parameters */
    if (args->width == 0 || args->height == 0 || args->bpp == 0)
        return -EINVAL;

    if (args->bpp != 8 && args->bpp != 16 && args->bpp != 24 && args->bpp != 32)
        return -EINVAL;

    if (args->width > 8192 || args->height > 8192)
        return -EINVAL;

    /* Calculate pitch and size */
    uint32_t pitch = dumb_calc_pitch(args->width, args->bpp);
    size_t size = (size_t)pitch * (size_t)args->height;

    args->pitch = pitch;
    args->size = (uint64_t)size;

    /* Create a GEM object */
    struct drm_gem_object *obj = NULL;
    int ret = drm_gem_create_object(size, 1, &obj);
    if (ret < 0) {
        kprintf("[DRM dumb] failed to allocate %llu bytes\n",
                (unsigned long long)size);
        return ret;
    }

    /* Create a userspace handle */
    uint32_t handle = 0;
    ret = drm_gem_handle_create(dev, obj, &handle);
    if (ret < 0) {
        drm_gem_unref(obj);
        return ret;
    }

    args->handle = handle;

    kprintf("[DRM dumb] created %ux%u (bpp=%u, pitch=%u, size=%llu, handle=%u)\n",
            args->width, args->height, args->bpp, pitch,
            (unsigned long long)size, handle);

    return 0;
}

/*
 * drm_dumb_map_offset — Get the mmap offset for a dumb buffer.
 *
 * @dev:   DRM device.
 * @args:  Arguments (handle in, offset out).
 *
 * Returns 0 on success.
 */
int drm_dumb_map_offset(struct drm_device *dev,
                         struct drm_mode_map_dumb *args)
{
    if (!dev || !args)
        return -1;

    /* Look up the GEM object by handle */
    struct drm_gem_object *obj = drm_gem_handle_lookup(dev, args->handle);
    if (!obj) {
        kprintf("[DRM dumb] invalid handle %u\n", args->handle);
        return -EINVAL;
    }

    /* Get the mmap offset (physical address) */
    uint64_t offset = 0;
    int ret = drm_gem_mmap(obj, &offset);
    if (ret < 0)
        return ret;

    args->offset = offset;
    return 0;
}

/*
 * drm_dumb_destroy — Destroy a dumb buffer.
 *
 * @dev:   DRM device.
 * @args:  Arguments (handle of buffer to destroy).
 *
 * Returns 0 on success.
 */
int drm_dumb_destroy(struct drm_device *dev,
                      struct drm_mode_destroy_dumb *args)
{
    if (!dev || !args)
        return -1;

    /* Close the GEM handle (drops reference, frees if last ref) */
    return drm_gem_handle_close(dev, args->handle);
}

/* ── Stub: drm_dumb_sync ─────────────────────────────── */
int drm_dumb_sync(void *file, void *dev, void *args)
{
    (void)file;
    (void)dev;
    (void)args;
    kprintf("[drm] drm_dumb_sync: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: drm_dumb_mmap ─────────────────────────────── */
int drm_dumb_mmap(void *file, void *dev, void *vma)
{
    (void)file;
    (void)dev;
    (void)vma;
    kprintf("[drm] drm_dumb_mmap: not yet implemented\n");
    return -ENOSYS;
}
