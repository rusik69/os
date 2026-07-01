#ifndef DRM_IRQ_H
#define DRM_IRQ_H

#include "types.h"

/* ═══════════════════════════════════════════════════════════════════
 *  DRM IRQ / vblank API
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Subsystem lifecycle ────────────────────────────────────────── */

int  drm_irq_init(void);
void drm_irq_exit(void);

/* ── Per-device vblank init / cleanup ───────────────────────────── */

int  drm_vblank_init(struct drm_device *dev, int num_crtcs);
void drm_vblank_cleanup(struct drm_device *dev);

/* ── Vblank enable / disable (refcounted) ───────────────────────── */

int  drm_vblank_get(struct drm_device *dev, int crtc_idx);
int  drm_vblank_put(struct drm_device *dev, int crtc_idx);

/* ── Vblank count query ─────────────────────────────────────────── */

uint64_t drm_vblank_count(struct drm_device *dev, int crtc_idx);

/* ── Handle a vblank event (called from IRQ or sim timer) ───────── */

void drm_handle_vblank(struct drm_device *dev, int crtc_idx);

/* ── WAIT_VBLANK ioctl handler ──────────────────────────────────── */

int drm_wait_vblank_ioctl(struct drm_device *dev, struct drm_file *fp,
			   void *arg);

/* ── GPU error IRQ reporting ────────────────────────────────────── */

void drm_irq_post_error(struct drm_device *dev, uint32_t error_flags);
void drm_irq_post_flip(struct drm_device *dev, int crtc_idx);

/* ── IRQ statistics ─────────────────────────────────────────────── */

struct drm_irq_stats {
	uint64_t vblank_count;      /* total vblank events processed */
	uint64_t error_count;       /* total GPU error events */
	uint64_t flip_count;        /* total page flips processed */
	uint64_t last_error_flags;  /* flags from most recent error */
	uint64_t last_error_time;   /* ktime of most recent error */
};

void drm_irq_get_stats(struct drm_irq_stats *out);
void drm_irq_reset_stats(void);

/* ── Hardware IRQ install / uninstall (for real GPU drivers) ──────── */

int  drm_irq_install(struct drm_device *dev, int irq_vector);
int  drm_irq_uninstall(struct drm_device *dev);

/* ── Generic IRQ handler (fallback) ─────────────────────────────── */

struct interrupt_frame;
void drm_irq_handler(struct interrupt_frame *frame);

#endif /* DRM_IRQ_H */
