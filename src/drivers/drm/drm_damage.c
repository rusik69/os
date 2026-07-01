/*
 * drm_damage.c — DRM damage tracking for efficient framebuffer updates
 *
 * Provides accumulated-bounding-box damage tracking for DRM framebuffers.
 * Userspace marks dirty rectangles via DRM_IOCTL_MODE_DIRTYFB; the
 * kernel accumulates them into a single damage rectangle that can be
 * queried by display drivers to flush only the changed region.
 *
 * Architecture:
 *   - Each struct drm_framebuffer carries damage_x1/y1/x2/y2 + damage_valid.
 *   - drm_damage_mark() merges a new rectangle into the existing damage,
 *     expanding the bounding box as needed.
 *   - Rectangles are clipped to the framebuffer dimensions.
 *   - drm_damage_clear() resets the damage region after a flush.
 *   - drm_damage_get_rect() returns the accumulated region for flush.
 *   - A global spinlock protects concurrent damage updates.
 *
 * Item D143 task 8 — DRM damage tracking for efficient updates
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────── */

static spinlock_t g_damage_lock;
static int        g_damage_initialized = 0;

/* ═══════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * damage_clip — Clip a rectangle to the framebuffer bounds.
 * Ensures (x1,y1) <= (x2,y2) and are within [0, width] × [0, height].
 */
static void damage_clip(struct drm_framebuffer *fb,
                         int *x1, int *y1, int *x2, int *y2)
{
    if (*x1 < 0)              *x1 = 0;
    if (*y1 < 0)              *y1 = 0;
    if (*x2 > (int)fb->width)  *x2 = (int)fb->width;
    if (*y2 > (int)fb->height) *y2 = (int)fb->height;

    /* Ensure non-negative dimensions */
    if (*x2 < *x1) *x2 = *x1;
    if (*y2 < *y1) *y2 = *y1;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * drm_damage_init — Initialise the damage tracking subsystem.
 *
 * Returns 0 on success, negative errno on failure.
 */
int drm_damage_init(void)
{
    spinlock_init(&g_damage_lock);
    g_damage_initialized = 1;

    kprintf("[DRM damage] tracking subsystem initialised\n");
    return 0;
}

/*
 * drm_damage_exit — Tear down the damage tracking subsystem.
 */
void drm_damage_exit(void)
{
    g_damage_initialized = 0;
    kprintf("[DRM damage] tracking subsystem shut down\n");
}

/*
 * drm_damage_init_fb — Initialise damage tracking for a framebuffer.
 *
 * @fb:  Target framebuffer (must not be NULL).
 */
void drm_damage_init_fb(struct drm_framebuffer *fb)
{
    if (!fb)
        return;

    spinlock_acquire(&g_damage_lock);

    fb->damage_valid = 0;
    fb->damage_x1 = 0;
    fb->damage_y1 = 0;
    fb->damage_x2 = 0;
    fb->damage_y2 = 0;

    spinlock_release(&g_damage_lock);
}

/*
 * drm_damage_mark — Mark a rectangular region as damaged.
 *
 * The rectangle is clipped to the framebuffer dimensions, then
 * merged into the accumulated damage bounding box.  If no damage
 * has been recorded yet for this fb, the rectangle becomes the
 * initial damage region.
 *
 * @fb:  Target framebuffer.
 * @x1:  Left edge (inclusive, pixels).
 * @y1:  Top edge (inclusive, pixels).
 * @x2:  Right edge (exclusive, pixels).
 * @y2:  Bottom edge (exclusive, pixels).
 */
void drm_damage_mark(struct drm_framebuffer *fb,
                      int x1, int y1, int x2, int y2)
{
    if (!fb || !fb->in_use)
        return;

    /* Clip to framebuffer bounds */
    damage_clip(fb, &x1, &y1, &x2, &y2);

    /* Empty rectangle — nothing to do */
    if (x1 >= x2 || y1 >= y2)
        return;

    spinlock_acquire(&g_damage_lock);

    if (fb->damage_valid) {
        /* Merge into existing damage bounding box */
        if (x1 < fb->damage_x1) fb->damage_x1 = x1;
        if (y1 < fb->damage_y1) fb->damage_y1 = y1;
        if (x2 > fb->damage_x2) fb->damage_x2 = x2;
        if (y2 > fb->damage_y2) fb->damage_y2 = y2;
    } else {
        /* First damage — set initial rectangle */
        fb->damage_x1 = x1;
        fb->damage_y1 = y1;
        fb->damage_x2 = x2;
        fb->damage_y2 = y2;
        fb->damage_valid = 1;
    }

    spinlock_release(&g_damage_lock);
}

/*
 * drm_damage_mark_whole — Mark the entire framebuffer as damaged.
 *
 * @fb:  Target framebuffer.
 */
void drm_damage_mark_whole(struct drm_framebuffer *fb)
{
    if (!fb || !fb->in_use)
        return;

    drm_damage_mark(fb, 0, 0, (int)fb->width, (int)fb->height);
}

/*
 * drm_damage_get_rect — Retrieve the accumulated damage rectangle.
 *
 * Returns 1 if damage has been recorded, 0 if the fb is clean.
 * Output pointers may be NULL to ignore specific values.
 *
 * @fb:   Target framebuffer.
 * @x1:   Receives left edge (may be NULL).
 * @y1:   Receives top edge (may be NULL).
 * @x2:   Receives right edge (may be NULL).
 * @y2:   Receives bottom edge (may be NULL).
 *
 * Returns 1 if damage recorded, 0 if clean, negative errno on error.
 */
int drm_damage_get_rect(struct drm_framebuffer *fb,
                         int *x1, int *y1, int *x2, int *y2)
{
    if (!fb)
        return -EINVAL;

    spinlock_acquire(&g_damage_lock);

    int valid = fb->damage_valid;

    if (valid && x1) *x1 = fb->damage_x1;
    if (valid && y1) *y1 = fb->damage_y1;
    if (valid && x2) *x2 = fb->damage_x2;
    if (valid && y2) *y2 = fb->damage_y2;

    spinlock_release(&g_damage_lock);

    return valid ? 1 : 0;
}

/*
 * drm_damage_clear — Clear (reset) damage tracking for a framebuffer.
 *
 * After calling this, the fb is considered clean (no pending damage).
 *
 * @fb:  Target framebuffer.
 */
void drm_damage_clear(struct drm_framebuffer *fb)
{
    if (!fb)
        return;

    spinlock_acquire(&g_damage_lock);

    fb->damage_valid = 0;
    fb->damage_x1 = 0;
    fb->damage_y1 = 0;
    fb->damage_x2 = 0;
    fb->damage_y2 = 0;

    spinlock_release(&g_damage_lock);
}
