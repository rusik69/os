/*
 * drm_irq.c — DRM GPU IRQ handling: vblank, error reporting
 *
 * Provides per-CRTC vblank counter management and GPU error IRQ handling.
 * Since most emulated GPUs (Bochs, simplefb) lack a real vblank interrupt,
 * this module uses a high-resolution timer (hrtimer) at ~60 Hz to simulate
 * vblank events.  Real hardware drivers override the timer with their own
 * interrupt handler via drm_irq_install() / drm_irq_uninstall().
 *
 * Architecture:
 *   - struct drm_vblank_crtc: per-CRTC vblank state (counter, timestamp,
 *     enable refcount, simulated timer)
 *   - drm_vblank_init() / drm_vblank_cleanup(): per-device setup/teardown
 *   - drm_wait_vblank_ioctl(): DRM_IOCTL_WAIT_VBLANK handler
 *   - drm_handle_vblank(): called by hardware IRQ or simulated timer to
 *     advance the counter and wake waiters
 *   - drm_irq_install() / drm_irq_uninstall(): real hardware IRQ binding
 *   - drm_irq_stats: global error/interrupt statistics
 *
 * Item D143 task 13 — GPU IRQ handling (vblank, error)
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "drm_irq.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"
#include "hrtimer.h"
#include "waitqueue.h"
#include "pmm.h"
#include "idt.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Internal per-CRTC vblank state
 * ═══════════════════════════════════════════════════════════════════ */

struct drm_vblank_crtc {
	int           in_use;       /* slot allocated */
	uint32_t      crtc_id;      /* owning CRTC id */
	uint64_t      count;        /* cumulative vblank counter */
	ktime_t       time;         /* ktime of last vblank event */
	uint64_t      last_wait;    /* seqno consumed by last WAIT_VBLANK */
	int           enabled;      /* vblank interrupt enabled */
	int           refcount;     /* vblank_get / vblank_put refcount */
	struct hrtimer timer;       /* simulated vblank timer (when no hw IRQ) */
	struct wait_queue waitq;    /* waiters on next vblank */
	struct drm_device *dev;     /* back-pointer to owning device */
};

/* ═══════════════════════════════════════════════════════════════════
 *  Global state
 * ═══════════════════════════════════════════════════════════════════ */

#define DRM_MAX_VBLANK_CRTCS  DRM_MAX_CRTC

static struct drm_vblank_crtc g_vblank_crtcs[DRM_MAX_VBLANK_CRTCS];
static int g_drm_irq_inited = 0;
static spinlock_t g_drm_irq_lock;

/* GPU IRQ statistics */
static struct drm_irq_stats g_irq_stats;
static spinlock_t g_irq_stats_lock;

/* Default simulated refresh interval (60 Hz) in nanoseconds */
#define VBLANK_DEFAULT_INTERVAL_NS 16666666ULL  /* ~60 Hz */

/* ═══════════════════════════════════════════════════════════════════
 *  Simulated vblank timer callback
 * ═══════════════════════════════════════════════════════════════════ */

static void drm_vblank_sim_timer_cb(void *data)
{
	struct drm_vblank_crtc *vc = (struct drm_vblank_crtc *)data;

	if (!vc || !vc->in_use || !vc->enabled)
		return;

	/* Advance the vblank counter and record the timestamp */
	spinlock_acquire(&g_drm_irq_lock);
	vc->count++;
	vc->time = ns_to_ktime(0);   /* 0 means "read current time"; we use
				       * the hrtimer expiry as a proxy */
	spinlock_release(&g_drm_irq_lock);

	/* Wake any waiters */
	wait_queue_wake(&vc->waitq);

	/* Re-arm the timer (directly via hrtimer_start since we are in
	 * softirq context; the timer will fire after the interval) */
	hrtimer_start(&vc->timer, VBLANK_DEFAULT_INTERVAL_NS);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Initialisation / cleanup
 * ═══════════════════════════════════════════════════════════════════ */

int drm_irq_init(void)
{
	if (g_drm_irq_inited)
		return 0;

	spinlock_init(&g_drm_irq_lock);
	spinlock_init(&g_irq_stats_lock);
	memset(g_vblank_crtcs, 0, sizeof(g_vblank_crtcs));
	memset(&g_irq_stats, 0, sizeof(g_irq_stats));
	g_drm_irq_inited = 1;

	kprintf("[DRM IRQ] vblank + error handling initialised\n");
	return 0;
}

void drm_irq_exit(void)
{
	if (!g_drm_irq_inited)
		return;

	spinlock_acquire(&g_drm_irq_lock);

	/* Cancel all simulated vblank timers */
	for (int i = 0; i < DRM_MAX_VBLANK_CRTCS; i++) {
		if (g_vblank_crtcs[i].in_use) {
			hrtimer_cancel(&g_vblank_crtcs[i].timer);
			g_vblank_crtcs[i].in_use = 0;
		}
	}

	spinlock_release(&g_drm_irq_lock);
	g_drm_irq_inited = 0;

	kprintf("[DRM IRQ] shutdown\n");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Per-device vblank initialisation
 * ═══════════════════════════════════════════════════════════════════ */

int drm_vblank_init(struct drm_device *dev, int num_crtcs)
{
	if (!dev || num_crtcs <= 0 || num_crtcs > DRM_MAX_VBLANK_CRTCS)
		return -EINVAL;

	int allocated = 0;

	spinlock_acquire(&g_drm_irq_lock);

	for (int i = 0; i < DRM_MAX_VBLANK_CRTCS && allocated < num_crtcs; i++) {
		if (!g_vblank_crtcs[i].in_use) {
			struct drm_vblank_crtc *vc = &g_vblank_crtcs[i];
			memset(vc, 0, sizeof(*vc));
			vc->in_use   = 1;
			vc->crtc_id  = (uint32_t)(i + 1); /* 1-based index */
			vc->count    = 0;
			vc->time     = 0;
			vc->last_wait = 0;
			vc->enabled  = 0;
			vc->refcount = 0;
			vc->dev      = dev;
			wait_queue_init(&vc->waitq);
			hrtimer_init(&vc->timer, drm_vblank_sim_timer_cb, vc);
			allocated++;
		}
	}

	spinlock_release(&g_drm_irq_lock);

	kprintf("[DRM IRQ] vblank init: %d CRTC(s) on '%s'\n",
		allocated, dev->name ? dev->name : "?");
	return (allocated == num_crtcs) ? 0 : -ENOSPC;
}

void drm_vblank_cleanup(struct drm_device *dev)
{
	if (!dev)
		return;

	spinlock_acquire(&g_drm_irq_lock);

	for (int i = 0; i < DRM_MAX_VBLANK_CRTCS; i++) {
		if (g_vblank_crtcs[i].in_use &&
		    g_vblank_crtcs[i].dev == dev) {
			hrtimer_cancel(&g_vblank_crtcs[i].timer);
			g_vblank_crtcs[i].in_use = 0;
		}
	}

	spinlock_release(&g_drm_irq_lock);

	kprintf("[DRM IRQ] vblank cleanup for '%s'\n",
		dev->name ? dev->name : "?");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Vblank enable / disable (refcounted)
 * ═══════════════════════════════════════════════════════════════════ */

int drm_vblank_get(struct drm_device *dev, int crtc_idx)
{
	(void)dev;

	if (crtc_idx < 0 || crtc_idx >= DRM_MAX_VBLANK_CRTCS)
		return -EINVAL;

	int ret = 0;
	spinlock_acquire(&g_drm_irq_lock);

	struct drm_vblank_crtc *vc = &g_vblank_crtcs[crtc_idx];
	if (!vc->in_use) {
		ret = -ENOENT;
		goto out;
	}

	vc->refcount++;
	if (vc->refcount == 1 && !vc->enabled) {
		/* Enable: start the simulated vblank timer */
		vc->enabled = 1;
		hrtimer_start(&vc->timer, VBLANK_DEFAULT_INTERVAL_NS);
		kprintf("[DRM IRQ] vblank %d: enabled (ref=%d)\n",
			crtc_idx, vc->refcount);
	}

out:
	spinlock_release(&g_drm_irq_lock);
	return ret;
}

int drm_vblank_put(struct drm_device *dev, int crtc_idx)
{
	(void)dev;

	if (crtc_idx < 0 || crtc_idx >= DRM_MAX_VBLANK_CRTCS)
		return -EINVAL;

	spinlock_acquire(&g_drm_irq_lock);

	struct drm_vblank_crtc *vc = &g_vblank_crtcs[crtc_idx];
	if (!vc->in_use || vc->refcount <= 0) {
		spinlock_release(&g_drm_irq_lock);
		return -EINVAL;
	}

	vc->refcount--;
	if (vc->refcount == 0 && vc->enabled) {
		/* Disable: stop the simulated timer */
		vc->enabled = 0;
		hrtimer_cancel(&vc->timer);
		kprintf("[DRM IRQ] vblank %d: disabled\n", crtc_idx);
	}

	spinlock_release(&g_drm_irq_lock);
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Handle a vblank event (called from IRQ or simulated timer)
 * ═══════════════════════════════════════════════════════════════════ */

void drm_handle_vblank(struct drm_device *dev, int crtc_idx)
{
	if (!dev || crtc_idx < 0 || crtc_idx >= DRM_MAX_VBLANK_CRTCS)
		return;

	spinlock_acquire(&g_drm_irq_lock);

	struct drm_vblank_crtc *vc = &g_vblank_crtcs[crtc_idx];
	if (!vc->in_use || vc->dev != dev) {
		spinlock_release(&g_drm_irq_lock);
		return;
	}

	vc->count++;
	vc->time = ns_to_ktime(0);  /* approximate timestamp */

	/* Update global stats */
	spinlock_acquire(&g_irq_stats_lock);
	g_irq_stats.vblank_count++;
	spinlock_release(&g_irq_stats_lock);

	spinlock_release(&g_drm_irq_lock);

	/* Wake waiters outside the lock */
	wait_queue_wake(&vc->waitq);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Query vblank counter
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t drm_vblank_count(struct drm_device *dev, int crtc_idx)
{
	(void)dev;

	if (crtc_idx < 0 || crtc_idx >= DRM_MAX_VBLANK_CRTCS)
		return 0;

	spinlock_acquire(&g_drm_irq_lock);
	uint64_t count = g_vblank_crtcs[crtc_idx].in_use
			? g_vblank_crtcs[crtc_idx].count : 0;
	spinlock_release(&g_drm_irq_lock);
	return count;
}

/* ═══════════════════════════════════════════════════════════════════
 *  WAIT_VBLANK ioctl handler
 * ═══════════════════════════════════════════════════════════════════ */

int drm_wait_vblank_ioctl(struct drm_device *dev, struct drm_file *fp,
			   void *arg)
{
	(void)fp;
	union drm_wait_vblank *vbl_wait = (union drm_wait_vblank *)arg;

	if (!dev || !vbl_wait)
		return -EINVAL;

	/* Extract CRTC index from the request type */
	int crtc_idx = (int)(vbl_wait->request.type & DRM_VBLANK_CRTC_MASK);

	if (crtc_idx < 0 || crtc_idx >= DRM_MAX_VBLANK_CRTCS)
		return -EINVAL;

	spinlock_acquire(&g_drm_irq_lock);

	struct drm_vblank_crtc *vc = &g_vblank_crtcs[crtc_idx];
	if (!vc->in_use || vc->dev != dev) {
		spinlock_release(&g_drm_irq_lock);
		return -ENOENT;
	}

	/* Ensure vblank is enabled (get reference) */
	if (!vc->enabled) {
		vc->refcount++;
		vc->enabled = 1;
		hrtimer_start(&vc->timer, VBLANK_DEFAULT_INTERVAL_NS);
		kprintf("[DRM IRQ] vblank %d: auto-enabled for WAIT_VBLANK\n",
			crtc_idx);
	}

	uint64_t target_seq = vbl_wait->request.sequence;
	uint64_t current_seq = vc->count;

	spinlock_release(&g_drm_irq_lock);

	/* If the caller asked for a specific sequence, wait until we
	 * reach it.  Otherwise wait for the next vblank (seq == 0). */
	if (target_seq == 0) {
		/* Wait for next vblank (current + 1) */
		target_seq = current_seq + 1;
	}

	/* Block until the counter reaches at least target_seq */
	/* Use a timeout of 1 second as safety (vblank at 60 Hz should
	 * arrive in ~16 ms; 1s is generous). */
	while (drm_vblank_count(dev, crtc_idx) < target_seq) {
		int ret = wait_queue_sleep_interruptible_timeout(
				&vc->waitq, 100); /* ticks ~ 1 sec */
		if (ret < 0) {
			/* Signal interrupted or timeout */
			vbl_wait->reply.sequence = (uint32_t)drm_vblank_count(dev, crtc_idx);
			vbl_wait->reply.tstamp_sec = 0;
			vbl_wait->reply.tstamp_usec = 0;
			return ret;
		}
	}

	/* Return the current vblank count and a best-effort timestamp */
	uint64_t now_count = drm_vblank_count(dev, crtc_idx);

	spinlock_acquire(&g_drm_irq_lock);
	ktime_t t = vc->time;
	spinlock_release(&g_drm_irq_lock);

	vbl_wait->reply.sequence    = (uint32_t)now_count;
	vbl_wait->reply.tstamp_sec  = (uint32_t)((uint64_t)t / 1000000000ULL);
	vbl_wait->reply.tstamp_usec = (uint32_t)(((uint64_t)t / 1000ULL) % 1000000ULL);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  GPU error IRQ handling
 * ═══════════════════════════════════════════════════════════════════ */

void drm_irq_post_error(struct drm_device *dev, uint32_t error_flags)
{
	if (!dev)
		return;

	spinlock_acquire(&g_irq_stats_lock);
	g_irq_stats.error_count++;
	g_irq_stats.last_error_flags = error_flags;
	g_irq_stats.last_error_time = ns_to_ktime(0);
	spinlock_release(&g_irq_stats_lock);

	kprintf("[DRM IRQ] GPU error on '%s': flags=0x%x (total=%llu)\n",
		dev->name ? dev->name : "?",
		(unsigned int)error_flags,
		(unsigned long long)g_irq_stats.error_count);
}

void drm_irq_post_flip(struct drm_device *dev, int crtc_idx)
{
	if (!dev)
		return;

	spinlock_acquire(&g_irq_stats_lock);
	g_irq_stats.flip_count++;
	spinlock_release(&g_irq_stats_lock);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Query IRQ statistics (for /proc or debug)
 * ═══════════════════════════════════════════════════════════════════ */

void drm_irq_get_stats(struct drm_irq_stats *out)
{
	if (!out)
		return;

	spinlock_acquire(&g_irq_stats_lock);
	memcpy(out, &g_irq_stats, sizeof(g_irq_stats));
	spinlock_release(&g_irq_stats_lock);
}

void drm_irq_reset_stats(void)
{
	spinlock_acquire(&g_irq_stats_lock);
	memset(&g_irq_stats, 0, sizeof(g_irq_stats));
	spinlock_release(&g_irq_stats_lock);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Hardware IRQ install / uninstall (for real GPU drivers)
 * ═══════════════════════════════════════════════════════════════════ */

int drm_irq_install(struct drm_device *dev, int irq_vector)
{
	if (!dev)
		return -EINVAL;

	/* Cancel the simulated timer for all CRTCs belonging to this
	 * device — a real IRQ handler will take over. */
	spinlock_acquire(&g_drm_irq_lock);

	for (int i = 0; i < DRM_MAX_VBLANK_CRTCS; i++) {
		if (g_vblank_crtcs[i].in_use &&
		    g_vblank_crtcs[i].dev == dev) {
			hrtimer_cancel(&g_vblank_crtcs[i].timer);
		}
	}

	spinlock_release(&g_drm_irq_lock);

	kprintf("[DRM IRQ] IRQ %d installed for '%s'\n",
		irq_vector, dev->name ? dev->name : "?");
	return 0;
}

int drm_irq_uninstall(struct drm_device *dev)
{
	if (!dev)
		return -EINVAL;

	/* Re-enable simulated timers for CRTCs that still want vblank */
	spinlock_acquire(&g_drm_irq_lock);

	for (int i = 0; i < DRM_MAX_VBLANK_CRTCS; i++) {
		if (g_vblank_crtcs[i].in_use &&
		    g_vblank_crtcs[i].dev == dev &&
		    g_vblank_crtcs[i].enabled) {
			hrtimer_start(&g_vblank_crtcs[i].timer,
				      VBLANK_DEFAULT_INTERVAL_NS);
		}
	}

	spinlock_release(&g_drm_irq_lock);

	kprintf("[DRM IRQ] IRQ uninstalled for '%s' (sim timers restored)\n",
		dev->name ? dev->name : "?");
	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic IRQ handler (for dispatching to driver-specific handlers)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * The actual IRQ handler for a real GPU driver is registered via
 * idt_register_handler() by the driver itself.  This generic handler
 * can be used as a fallback that only logs errors.
 */
void drm_irq_handler(struct interrupt_frame *frame)
{
	(void)frame;
	kprintf("[DRM IRQ] unhandled interrupt\n");
}
