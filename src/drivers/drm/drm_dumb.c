/*
 * drm_dumb.c — DRM dumb buffer allocation
 *
 * Implements DRM_IOCTL_MODE_CREATE_DUMB, DRM_IOCTL_MODE_MAP_DUMB,
 * DRM_IOCTL_MODE_DESTROY_DUMB, and DRM_IOCTL_MODE_DIRTYFB.
 * Dumb buffers are simple framebuffers allocated via GEM, used by
 * fbdev emulation and simple DRM clients.
 *
 * A dumb buffer is a GEM-backed buffer object created with a
 * simple linear layout (no tiling or compression).
 *
 * Drivers may override dumb buffer allocation via the
 * struct drm_driver dumb_create / dumb_map_offset / dumb_destroy
 * callbacks (e.g. bochs uses LFB memory for scanout).
 *
 * Item S25 — DRM dumb buffer
 * Task D143-5 — DRM dumb buffer allocation (stub → real)
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"
#include "vmm.h"

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
 * requested resolution and bpp.  If the driver provides a
 * dumb_create callback, it is used instead (e.g. for LFB-backed
 * allocation).  Returns the GEM handle and pitch.
 *
 * @dev:   DRM device.
 * @args:  Arguments from userspace (height, width, bpp).
 *         Returns handle, pitch, size.
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_dumb_create(struct drm_device *dev,
                     struct drm_mode_create_dumb *args)
{
    if (!dev || !args)
        return -EINVAL;

    /* Allow driver-specific allocation first */
    if (dev->driver && dev->driver->dumb_create)
        return dev->driver->dumb_create(dev, args);

    /* Validate parameters */
    if (args->width == 0 || args->height == 0 || args->bpp == 0)
        return -EINVAL;

    if (args->bpp != 8 && args->bpp != 16 && args->bpp != 24 &&
        args->bpp != 32)
        return -EINVAL;

    if (args->width > 8192 || args->height > 8192)
        return -EINVAL;

    if (args->flags != 0)
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

    kprintf("[DRM dumb] created %ux%u (bpp=%u, pitch=%u, "
            "size=%llu, handle=%u)\n",
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
 * Returns 0 on success, negative errno on failure.
 */
int drm_dumb_map_offset(struct drm_device *dev,
                         struct drm_mode_map_dumb *args)
{
    if (!dev || !args)
        return -EINVAL;

    /* Allow driver-specific map */
    if (dev->driver && dev->driver->dumb_map_offset)
        return dev->driver->dumb_map_offset(dev, args);

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
 * Returns 0 on success, negative errno on failure.
 */
int drm_dumb_destroy(struct drm_device *dev,
                      struct drm_mode_destroy_dumb *args)
{
    if (!dev || !args)
        return -EINVAL;

    /* Allow driver-specific destroy */
    if (dev->driver && dev->driver->dumb_destroy)
        return dev->driver->dumb_destroy(dev, args);

    /* Close the GEM handle (drops reference, frees if last ref) */
    int ret = drm_gem_handle_close(dev, args->handle);
    if (ret < 0) {
        kprintf("[DRM dumb] failed to close handle %u\n", args->handle);
        return -EINVAL;
    }
    return 0;
}

/*
 * drm_dumb_mmap — Get the kernel virtual address for a GEM object
 *                 backing a dumb buffer.
 *
 * Used internally when the kernel needs to access dumb buffer pixels
 * directly (e.g. software rendering, fbcon).
 *
 * @obj:   GEM object backing the dumb buffer.
 * @vaddr: Receives the kernel virtual address of the buffer.
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_dumb_mmap(struct drm_gem_object *obj, void **vaddr)
{
    if (!obj || !vaddr)
        return -EINVAL;

    /* If the GEM object already has a kernel mapping, use it */
    if (obj->vaddr) {
        *vaddr = obj->vaddr;
        return 0;
    }

    /* No existing mapping — map the physical pages */
    if (obj->phys_addr == 0)
        return -ENOMEM;

    void *map = vmm_map_phys(obj->phys_addr, obj->size,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (!map)
        return -ENOMEM;

    obj->vaddr = map;
    *vaddr = map;
    return 0;
}

/*
 * drm_dumb_dirtyfb — Mark a framebuffer as dirty (region updated).
 *
 * Handles DRM_IOCTL_MODE_DIRTYFB.  When clip rectangles are
 * provided, each is merged into the framebuffer's accumulated
 * damage region.  When no clips are given, the entire framebuffer
 * is marked damaged.  Display drivers query the accumulated damage
 * via drm_damage_get_rect() to flush only changed regions.
 *
 * @dev:  DRM device.
 * @args: Dirty rectangle arguments (fb_id, clip rects).
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_dumb_dirtyfb(struct drm_device *dev,
                      struct drm_mode_fb_dirty_cmd *args)
{
    if (!dev || !args)
        return -EINVAL;

    /* Validate that the framebuffer exists */
    struct drm_framebuffer *fb = drm_fb_lookup(dev, args->fb_id);
    if (!fb) {
        kprintf("[DRM dumb] dirtyfb: invalid fb_id %u\n", args->fb_id);
        return -EINVAL;
    }

    /* If no clip rects, mark the whole fb as dirty */
    if (args->num_clips == 0 || args->clips_ptr == 0) {
        kprintf("[DRM damage] dirtyfb %u: full-screen update\n",
                args->fb_id);
        drm_damage_mark_whole(fb);
        return 0;
    }

    /* Walk the clip rectangle list from userspace and accumulate
     * damage.  Each clip rect is a struct drm_clip_rect:
     *   { uint16_t x1, y1, x2, y2; }
     * where (x1,y1) is the upper-left inclusive corner and
     * (x2,y2) is the lower-right exclusive corner.
     */
    uint32_t num_clips = args->num_clips;
    if (num_clips > 1024)
        num_clips = 1024;  /* sanity cap */

    for (uint32_t i = 0; i < num_clips; i++) {
        /* Read clip rect from userspace one at a time.
         * Using a simple struct copy from userspace. */
        struct {
            uint16_t x1, y1, x2, y2;
        } clip;
        uint64_t addr = args->clips_ptr + (uint64_t)i * sizeof(clip);

        /* Copy from userspace — in a real kernel this would use
         * copy_from_user().  For our kernel memory model with
         * identity-mapped userspace, a direct dereference works. */
        const struct { uint16_t x1, y1, x2, y2; } *src =
            (const void *)(uintptr_t)addr;
        clip.x1 = src->x1;
        clip.y1 = src->y1;
        clip.x2 = src->x2;
        clip.y2 = src->y2;

        /* Merge this clip into the accumulated damage region */
        drm_damage_mark(fb,
                         (int)clip.x1, (int)clip.y1,
                         (int)clip.x2, (int)clip.y2);
    }

    kprintf("[DRM damage] dirtyfb %u: %u clip rect(s) accumulated\n",
            args->fb_id, num_clips);

    return 0;
}

/*
 * drm_dumb_sync — Synchronise (flush) a dumb buffer.
 *
 * For linear framebuffers with cache-coherent architectures
 * this is a no-op.  On non-coherent systems this would flush
 * the CPU cache for the buffer range.
 */
static int drm_dumb_sync(void *file, void *dev, void *args)
{
    (void)file;
    (void)dev;
    (void)args;
    /* Dumb buffers are linear and usually mapped WC or UC —
     * no explicit sync needed.  This is consistent with the
     * Linux DRM dumb buffer implementation. */
    return 0;
}
