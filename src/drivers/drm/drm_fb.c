/*
 * drm_fb.c — DRM framebuffer reference counting
 *
 * Provides reference-counted framebuffer management to prevent
 * premature deallocation while CRTCs, planes, or pending page-flips
 * still reference a framebuffer.
 *
 * Architecture:
 *   - drm_fb_ref() / drm_fb_unref() manage the refcount on a
 *     struct drm_framebuffer.
 *   - When refcount reaches 0, the framebuffer slot is freed
 *     (in_use = 0) and the underlying GEM handle is released.
 *   - drm_add_fb() allocates a slot with refcount = 1 (the
 *     userspace caller's reference).
 *   - drm_remove_fb() drops one reference via drm_fb_unref().
 *   - CRTC atomic commit takes an extra reference when a
 *     framebuffer is assigned to a CRTC.
 *
 * Item D143 task 3 — DRM framebuffer reference counting
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Reference counting
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_fb_ref — Take an additional reference on a framebuffer.
 *
 * Prevents the framebuffer from being freed while it is still
 * referenced (e.g. by a CRTC or a pending page-flip).
 *
 * @fb: Framebuffer to reference (must not be NULL).
 */
void drm_fb_ref(struct drm_framebuffer *fb)
{
    if (!fb || !fb->in_use)
        return;

    fb->refcount++;
    kprintf("[DRM fb] ref %u -> refcount=%d\n",
            fb->fb_id, fb->refcount);
}

/*
 * drm_fb_unref — Release a reference on a framebuffer.
 *
 * Decrements the refcount.  When refcount reaches 0, the framebuffer
 * slot is freed (in_use = 0) and the associated GEM handle is closed.
 *
 * @dev: DRM device (needed for GEM handle close).
 * @fb:  Framebuffer to release (may be NULL).
 */
void drm_fb_unref(struct drm_device *dev, struct drm_framebuffer *fb)
{
    if (!fb || !fb->in_use)
        return;

    if (fb->refcount <= 0) {
        /* Already freed or corrupt — belt-and-suspenders */
        kprintf("[DRM fb] WARNING: ref %u already at 0\n", fb->fb_id);
        return;
    }

    fb->refcount--;
    kprintf("[DRM fb] unref %u -> refcount=%d\n",
            fb->fb_id, fb->refcount);

    if (fb->refcount > 0)
        return;

    /* Refcount reached 0 — free the framebuffer slot */
    uint32_t handle = fb->handle;
    uint32_t fb_id  = fb->fb_id;

    memset(fb, 0, sizeof(*fb));

    kprintf("[DRM fb] freed framebuffer %u (handle %u)\n",
            fb_id, handle);

    /* Release the underlying GEM handle.  This drops the GEM
     * object's reference and frees the backing memory if no
     * other handles reference it. */
    if (handle != 0)
        drm_gem_handle_close(dev, handle);
}
