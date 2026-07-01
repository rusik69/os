/*
 * drm_fence.c — DRM DMA Fence Sync
 *
 * Implements DMA fence synchronisation primitives for tracking GPU
 * operation completion.  Fences are the core building block for
 * CPU-GPU and GPU-GPU synchronisation, used by command submission,
 * buffer sharing, vblank synchronisation, and atomic page flip
 * completion tracking.
 *
 * Architecture:
 *   - struct dma_fence: a single fence with seqno, context, signal
 *     state, waitqueue, and optional completion callbacks.
 *   - Fixed-size global pool (DMA_FENCE_MAX_FENCES) for fast
 *     allocation without heap dependency in IRQ context.
 *   - Each fence carries a refcount; freed when refcount reaches 0.
 *   - Callbacks are invoked synchronously inside dma_fence_signal().
 *
 * Item D143 task 14 — GPU DMA fence sync
 */

#define KERNEL_INTERNAL
#include "drm.h"
#include "drm_fence.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "pmm.h"
#include "errno.h"
#include "heap.h"
#include "waitqueue.h"
#include "hrtimer.h"
#include "timer.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════════════ */

/** Maximum number of fence objects in the global pool. */
#define DMA_FENCE_MAX_FENCES      256

/* ═══════════════════════════════════════════════════════════════════
 *  Global state
 * ═══════════════════════════════════════════════════════════════════ */

/** Static pool of fence slots. */
static struct dma_fence g_fences[DMA_FENCE_MAX_FENCES];

/** Global fence subsystem lock. */
static spinlock_t g_fence_lock;

/** Subsystem initialised flag. */
static int g_fence_inited = 0;

/** Global context counter — incremented for each dma_fence_context_alloc(). */
static uint64_t g_next_context = 1;

/* ═══════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * fence_find_slot — Find a free slot in the fence pool.
 * Returns index or -1 if full.
 */
static int fence_find_slot(void)
{
	for (int i = 0; i < DMA_FENCE_MAX_FENCES; i++) {
		if (!g_fences[i].in_use)
			return i;
	}
	return -1;
}

/**
 * fence_fire_callbacks — Invoke all registered callbacks.
 * Must be called with g_fence_lock held.
 */
static void fence_fire_callbacks(struct dma_fence *fence)
{
	for (int i = 0; i < fence->num_callbacks; i++) {
		if (fence->callbacks[i]) {
			dma_fence_cb_t cb = fence->callbacks[i];
			void *data = fence->callback_data[i];
			fence->callbacks[i] = NULL;
			fence->callback_data[i] = NULL;
			/* Release lock while calling out to the callback,
			 * since the callback may itself acquire the fence
			 * lock (e.g. to signal a dependent fence). */
			spinlock_release(&g_fence_lock);
			if (cb)
				cb(fence, data);
			spinlock_acquire(&g_fence_lock);
		}
	}
	fence->num_callbacks = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Subsystem lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

int drm_fence_init(void)
{
	if (g_fence_inited)
		return 0;

	spinlock_init(&g_fence_lock);

	memset(g_fences, 0, sizeof(g_fences));
	g_next_context = 1;

	g_fence_inited = 1;

	kprintf("[DRM fence] DMA fence sync subsystem initialised\n");
	return 0;
}

void drm_fence_exit(void)
{
	if (!g_fence_inited)
		return;

	spinlock_acquire(&g_fence_lock);

	/* Free any fences still in use (force-release). */
	for (int i = 0; i < DMA_FENCE_MAX_FENCES; i++) {
		if (g_fences[i].in_use) {
			/* Wake any stuck waiters so they don't block
			 * forever after subsystem teardown. */
			wait_queue_wake_all(&g_fences[i].waitq);
			memset(&g_fences[i], 0,
			       sizeof(struct dma_fence));
		}
	}

	g_fence_inited = 0;

	spinlock_release(&g_fence_lock);

	kprintf("[DRM fence] DMA fence sync subsystem shut down\n");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Context management
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t dma_fence_context_alloc(void)
{
	uint64_t ctx;

	spinlock_acquire(&g_fence_lock);
	ctx = g_next_context++;
	spinlock_release(&g_fence_lock);

	return ctx;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Fence creation / destruction
 * ═══════════════════════════════════════════════════════════════════ */

struct dma_fence *dma_fence_create(uint64_t context, uint64_t seqno)
{
	if (!g_fence_inited)
		return NULL;

	spinlock_acquire(&g_fence_lock);

	int idx = fence_find_slot();
	if (idx < 0) {
		spinlock_release(&g_fence_lock);
		kprintf("[DRM fence] WARNING: fence pool exhausted "
		        "(max %d)\n", DMA_FENCE_MAX_FENCES);
		return NULL;
	}

	struct dma_fence *fence = &g_fences[idx];

	memset(fence, 0, sizeof(*fence));
	fence->in_use   = 1;
	fence->context  = context;
	fence->seqno    = seqno;
	fence->flags    = 0;
	fence->error    = 0;
	fence->timestamp = 0;
	fence->refcount = 1;
	fence->num_callbacks = 0;

	wait_queue_init(&fence->waitq);

	spinlock_release(&g_fence_lock);

	kprintf("[DRM fence] create: ctx=%llu seqno=%llu "
	        "(fence=%p)\n",
	        (unsigned long long)context,
	        (unsigned long long)seqno,
	        (void *)fence);

	return fence;
}

void dma_fence_free(struct dma_fence *fence)
{
	if (!fence)
		return;

	spinlock_acquire(&g_fence_lock);

	fence->in_use   = 0;
	fence->context  = 0;
	fence->seqno    = 0;
	fence->flags    = 0;
	fence->error    = 0;
	fence->timestamp = 0;
	fence->refcount = 0;
	fence->num_callbacks = 0;

	/* Wake any remaining waiters (should not happen if refcount
	 * discipline is followed, but be safe). */
	wait_queue_wake_all(&fence->waitq);

	spinlock_release(&g_fence_lock);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Reference counting
 * ═══════════════════════════════════════════════════════════════════ */

struct dma_fence *dma_fence_get(struct dma_fence *fence)
{
	if (!fence)
		return NULL;

	spinlock_acquire(&g_fence_lock);
	if (fence->in_use && fence->refcount > 0) {
		fence->refcount++;
	}
	spinlock_release(&g_fence_lock);

	return fence;
}

void dma_fence_put(struct dma_fence *fence)
{
	if (!fence)
		return;

	int should_free = 0;

	spinlock_acquire(&g_fence_lock);

	if (fence->in_use && fence->refcount > 0) {
		fence->refcount--;
		if (fence->refcount == 0) {
			should_free = 1;
		}
	}

	spinlock_release(&g_fence_lock);

	if (should_free) {
		dma_fence_free(fence);
	}
}

/* ═══════════════════════════════════════════════════════════════════
 *  Signaling
 * ═══════════════════════════════════════════════════════════════════ */

int dma_fence_signal(struct dma_fence *fence)
{
	if (!fence)
		return -EINVAL;

	spinlock_acquire(&g_fence_lock);

	if (!fence->in_use) {
		spinlock_release(&g_fence_lock);
		return -ENOENT;
	}

	/* Idempotent — already signaled. */
	if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
		spinlock_release(&g_fence_lock);
		return 0;
	}

	/* Mark as signaled and record the timestamp. */
	fence->flags |= DMA_FENCE_FLAG_SIGNALED;
	fence->timestamp = ns_to_ktime(0);  /* current ktime in ns */
	fence->error = 0;

	/* Fire callbacks (releases lock temporarily for each). */
	fence_fire_callbacks(fence);

	/* Wake all waiters. */
	wait_queue_wake_all(&fence->waitq);

	spinlock_release(&g_fence_lock);

	kprintf("[DRM fence] signal: ctx=%llu seqno=%llu\n",
	        (unsigned long long)fence->context,
	        (unsigned long long)fence->seqno);

	return 0;
}

int dma_fence_signal_error(struct dma_fence *fence, int error)
{
	if (!fence)
		return -EINVAL;

	spinlock_acquire(&g_fence_lock);

	if (!fence->in_use) {
		spinlock_release(&g_fence_lock);
		return -ENOENT;
	}

	/* Idempotent. */
	if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
		spinlock_release(&g_fence_lock);
		return 0;
	}

	/* Mark as signaled with error. */
	fence->flags |= DMA_FENCE_FLAG_SIGNALED;
	fence->flags |= DMA_FENCE_FLAG_ERROR;
	fence->timestamp = ns_to_ktime(0);
	fence->error = error;

	/* Fire callbacks. */
	fence_fire_callbacks(fence);

	/* Wake all waiters. */
	wait_queue_wake_all(&fence->waitq);

	spinlock_release(&g_fence_lock);

	kprintf("[DRM fence] signal_error: ctx=%llu seqno=%llu "
	        "error=%d\n",
	        (unsigned long long)fence->context,
	        (unsigned long long)fence->seqno,
	        error);

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Query
 * ═══════════════════════════════════════════════════════════════════ */

int dma_fence_is_signaled(struct dma_fence *fence)
{
	if (!fence)
		return 0;

	spinlock_acquire(&g_fence_lock);
	int signaled = (fence->in_use &&
	               (fence->flags & DMA_FENCE_FLAG_SIGNALED)) ? 1 : 0;
	spinlock_release(&g_fence_lock);

	return signaled;
}

uint64_t dma_fence_get_seqno(struct dma_fence *fence)
{
	if (!fence)
		return 0;

	spinlock_acquire(&g_fence_lock);
	uint64_t seqno = fence->in_use ? fence->seqno : 0;
	spinlock_release(&g_fence_lock);

	return seqno;
}

uint64_t dma_fence_get_context(struct dma_fence *fence)
{
	if (!fence)
		return 0;

	spinlock_acquire(&g_fence_lock);
	uint64_t ctx = fence->in_use ? fence->context : 0;
	spinlock_release(&g_fence_lock);

	return ctx;
}

int dma_fence_get_status(struct dma_fence *fence)
{
	if (!fence)
		return -EINVAL;

	spinlock_acquire(&g_fence_lock);

	int status;
	if (!fence->in_use) {
		status = -ENOENT;
	} else if (!(fence->flags & DMA_FENCE_FLAG_SIGNALED)) {
		status = -EINPROGRESS;  /* -115: operation in progress */
	} else if (fence->flags & DMA_FENCE_FLAG_ERROR) {
		status = fence->error;
	} else {
		status = 0;  /* success */
	}

	spinlock_release(&g_fence_lock);

	return status;
}

uint64_t dma_fence_get_timestamp(struct dma_fence *fence)
{
	if (!fence)
		return 0;

	spinlock_acquire(&g_fence_lock);
	uint64_t ts = fence->in_use ? fence->timestamp : 0;
	spinlock_release(&g_fence_lock);

	return ts;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Wait operations
 * ═══════════════════════════════════════════════════════════════════ */

int dma_fence_wait(struct dma_fence *fence)
{
	if (!fence)
		return -EINVAL;

	/* Fast path: already signaled. */
	if (dma_fence_is_signaled(fence))
		return 0;

	kprintf("[DRM fence] wait: ctx=%llu seqno=%llu (blocking)\n",
	        (unsigned long long)fence->context,
	        (unsigned long long)fence->seqno);

	/* Slow path: block until signaled or interrupted. */
	while (!dma_fence_is_signaled(fence)) {
		int ret = wait_queue_sleep_interruptible(&fence->waitq);
		if (ret < 0) {
			kprintf("[DRM fence] wait interrupted: "
			        "ctx=%llu seqno=%llu ret=%d\n",
			        (unsigned long long)fence->context,
			        (unsigned long long)fence->seqno, ret);
			return ret;  /* -EINTR */
		}
	}

	return 0;
}

int dma_fence_wait_timeout(struct dma_fence *fence, uint64_t timeout_ns)
{
	if (!fence)
		return -EINVAL;

	/* Fast path: already signaled. */
	if (dma_fence_is_signaled(fence))
		return 0;

	/* Convert ns timeout to ticks. */
	uint64_t timeout_ticks = timeout_ns / NS_PER_TICK;
	if (timeout_ticks == 0)
		timeout_ticks = 1;  /* minimum 1 tick */

	uint64_t deadline = timer_get_ticks() + timeout_ticks;

	kprintf("[DRM fence] wait_timeout: ctx=%llu seqno=%llu "
	        "timeout=%lluns (%llu ticks)\n",
	        (unsigned long long)fence->context,
	        (unsigned long long)fence->seqno,
	        (unsigned long long)timeout_ns,
	        (unsigned long long)timeout_ticks);

	/* Slow path: block with timeout. */
	while (!dma_fence_is_signaled(fence)) {
		uint64_t now = timer_get_ticks();
		if (now >= deadline) {
			kprintf("[DRM fence] wait_timeout: TIMEOUT "
			        "ctx=%llu seqno=%llu\n",
			        (unsigned long long)fence->context,
			        (unsigned long long)fence->seqno);
			return -ETIME;
		}

		uint64_t remaining = deadline - now;
		int ret = wait_queue_sleep_interruptible_timeout(
			&fence->waitq, remaining);

		if (ret < 0 && ret != -ETIME) {
			kprintf("[DRM fence] wait_timeout interrupted: "
			        "ctx=%llu seqno=%llu ret=%d\n",
			        (unsigned long long)fence->context,
			        (unsigned long long)fence->seqno, ret);
			return ret;  /* -EINTR */
		}
	}

	return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Callback registration
 * ═══════════════════════════════════════════════════════════════════ */

int dma_fence_add_callback(struct dma_fence *fence,
                           dma_fence_cb_t cb, void *data)
{
	if (!fence || !cb)
		return -EINVAL;

	spinlock_acquire(&g_fence_lock);

	if (!fence->in_use) {
		spinlock_release(&g_fence_lock);
		return -ENOENT;
	}

	/* If the fence is already signaled, fire the callback
	 * synchronously and return. */
	if (fence->flags & DMA_FENCE_FLAG_SIGNALED) {
		spinlock_release(&g_fence_lock);
		cb(fence, data);
		return 0;
	}

	/* Find a free callback slot. */
	if (fence->num_callbacks >= DMA_FENCE_MAX_CALLBACKS) {
		spinlock_release(&g_fence_lock);
		return -ENOSPC;
	}

	fence->callbacks[fence->num_callbacks] = cb;
	fence->callback_data[fence->num_callbacks] = data;
	fence->num_callbacks++;

	/* Enable signaling flag. */
	fence->flags |= DMA_FENCE_FLAG_ENABLE_SIGNAL;

	spinlock_release(&g_fence_lock);

	return 0;
}

int dma_fence_remove_callback(struct dma_fence *fence,
                              dma_fence_cb_t cb, void *data)
{
	if (!fence || !cb)
		return -EINVAL;

	spinlock_acquire(&g_fence_lock);

	if (!fence->in_use) {
		spinlock_release(&g_fence_lock);
		return -ENOENT;
	}

	for (int i = 0; i < fence->num_callbacks; i++) {
		if (fence->callbacks[i] == cb &&
		    fence->callback_data[i] == data) {
			/* Remove by shifting remaining entries. */
			for (int j = i; j < fence->num_callbacks - 1; j++) {
				fence->callbacks[j] = fence->callbacks[j + 1];
				fence->callback_data[j] =
					fence->callback_data[j + 1];
			}
			fence->num_callbacks--;
			fence->callbacks[fence->num_callbacks] = NULL;
			fence->callback_data[fence->num_callbacks] = NULL;

			spinlock_release(&g_fence_lock);
			return 0;
		}
	}

	spinlock_release(&g_fence_lock);
	return -ENOENT;
}
