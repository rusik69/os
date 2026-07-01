#ifndef DRM_FENCE_H
#define DRM_FENCE_H

#include "types.h"
#include "waitqueue.h"

/* ═══════════════════════════════════════════════════════════════════
 *  DRM DMA Fence Synchronisation
 *
 *  DMA fences track GPU operation completion.  Each fence belongs to
 *  a fence context (a timeline), carries a monotonically increasing
 *  sequence number, and transitions from unsignaled → signaled when
 *  the GPU completes the associated operation.
 *
 *  Architecture:
 *    - struct dma_fence: a single fence with seqno, context, signal
 *      state, waitqueue, and optional completion callbacks.
 *    - dma_fence_context_alloc(): allocate a unique timeline context.
 *    - dma_fence_create(): instantiate a fence on a context.
 *    - dma_fence_signal(): mark fence as completed (wakes waiters).
 *    - dma_fence_wait() / dma_fence_wait_timeout(): block until
 *      signaled.
 *    - dma_fence_add_callback(): register a completion callback.
 *
 *  Item D143 task 14 — GPU DMA fence sync
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Fence flags ───────────────────────────────────────────────── */

/** Fence has been signaled (completed) */
#define DMA_FENCE_FLAG_SIGNALED      (1U << 0)

/** Signaling has been enabled for this fence */
#define DMA_FENCE_FLAG_ENABLE_SIGNAL (1U << 1)

/** Fence completed with an error */
#define DMA_FENCE_FLAG_ERROR         (1U << 2)

/* ── Maximum callbacks per fence ───────────────────────────────── */

#define DMA_FENCE_MAX_CALLBACKS      8

/* ── Default wait timeout (10 seconds in ns) ────────────────────── */

#define DMA_FENCE_DEFAULT_TIMEOUT_NS 10000000000ULL

/* ── Forward declarations ──────────────────────────────────────── */

struct dma_fence;

/** Callback function type for fence completion notification. */
typedef void (*dma_fence_cb_t)(struct dma_fence *fence, void *data);

/* ═══════════════════════════════════════════════════════════════════
 *  DMA fence structure
 * ═══════════════════════════════════════════════════════════════════ */

struct dma_fence {
    int      in_use;         /* slot allocated */
    uint64_t context;        /* fence context (timeline identifier) */
    uint64_t seqno;          /* sequence number within the context */
    uint32_t flags;          /* DMA_FENCE_FLAG_* */
    int      error;          /* error code if DMA_FENCE_FLAG_ERROR set */
    uint64_t timestamp;      /* ktime (ns) when signaled, 0 if unsignaled */

    /* Wait queue for blocking waiters */
    struct wait_queue waitq;

    /* Completion callbacks */
    dma_fence_cb_t callbacks[DMA_FENCE_MAX_CALLBACKS];
    void          *callback_data[DMA_FENCE_MAX_CALLBACKS];
    int            num_callbacks;

    /* Reference count (free when 0) */
    int refcount;
};

/* ═══════════════════════════════════════════════════════════════════
 *  Subsystem lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

/** Initialise the DMA fence subsystem.  Called once during DRM init. */
int  drm_fence_init(void);

/** Shut down the DMA fence subsystem.  Called during DRM exit. */
void drm_fence_exit(void);

/* ═══════════════════════════════════════════════════════════════════
 *  Context management
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_context_alloc — Allocate a new unique fence context.
 *
 * Returns a monotonically increasing 64-bit context identifier.
 * Contexts are used to distinguish different timelines (e.g. render
 * engine, copy engine, display engine).
 */
uint64_t dma_fence_context_alloc(void);

/* ═══════════════════════════════════════════════════════════════════
 *  Fence creation / destruction
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_create — Create a new fence on a given context.
 *
 * @context: Context/timeline identifier from dma_fence_context_alloc().
 * @seqno:   Sequence number (should be monotonically increasing per context).
 *
 * Returns a pointer to the new fence, or NULL on failure.
 * The fence starts in the unsignaled state with refcount=1.
 */
struct dma_fence *dma_fence_create(uint64_t context, uint64_t seqno);

/**
 * dma_fence_free — Release a fence and its resources.
 *
 * Called automatically when refcount reaches 0 via dma_fence_put().
 * Can also be called directly if you hold the only reference.
 */
void dma_fence_free(struct dma_fence *fence);

/* ═══════════════════════════════════════════════════════════════════
 *  Reference counting
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_get — Take an additional reference on a fence.
 *
 * Returns the fence pointer for convenience.
 */
struct dma_fence *dma_fence_get(struct dma_fence *fence);

/**
 * dma_fence_put — Release a reference on a fence.
 *
 * When the refcount reaches 0, the fence is freed via dma_fence_free().
 */
void dma_fence_put(struct dma_fence *fence);

/* ═══════════════════════════════════════════════════════════════════
 *  Signaling
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_signal — Mark a fence as signaled (completed).
 *
 * Wakes any waiters and fires registered completion callbacks.
 * If the fence was already signaled, returns 0 (idempotent).
 *
 * Returns 0 on success, negative errno on error.
 */
int dma_fence_signal(struct dma_fence *fence);

/**
 * dma_fence_signal_error — Mark a fence as signaled with an error.
 *
 * Like dma_fence_signal, but records an error code so that waiters
 * can distinguish successful completion from failure.
 *
 * @fence: The fence to signal.
 * @error: Negative errno value describing the failure.
 *
 * Returns 0 on success, negative errno on error.
 */
int dma_fence_signal_error(struct dma_fence *fence, int error);

/* ═══════════════════════════════════════════════════════════════════
 *  Query
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_is_signaled — Check whether a fence has been signaled.
 *
 * Returns 1 if signaled, 0 otherwise.
 */
int dma_fence_is_signaled(struct dma_fence *fence);

/**
 * dma_fence_get_seqno — Return the fence's sequence number.
 */
uint64_t dma_fence_get_seqno(struct dma_fence *fence);

/**
 * dma_fence_get_context — Return the fence's context identifier.
 */
uint64_t dma_fence_get_context(struct dma_fence *fence);

/**
 * dma_fence_get_status — Return the fence's completion status.
 *
 * Returns 0 if signaled successfully, negative errno if signaled
 * with error, -EINPROGRESS if not yet signaled.
 */
int dma_fence_get_status(struct dma_fence *fence);

/**
 * dma_fence_get_timestamp — Return the fence's signal timestamp (ns).
 *
 * Returns 0 if the fence is not yet signaled.
 */
uint64_t dma_fence_get_timestamp(struct dma_fence *fence);

/* ═══════════════════════════════════════════════════════════════════
 *  Wait operations
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_wait — Wait indefinitely for a fence to signal.
 *
 * Returns 0 on success (fence signaled), negative errno if the wait
 * was interrupted by a signal.
 */
int dma_fence_wait(struct dma_fence *fence);

/**
 * dma_fence_wait_timeout — Wait for a fence with a timeout.
 *
 * @fence:       The fence to wait on.
 * @timeout_ns:  Timeout in nanoseconds (0 = return immediately).
 *
 * Returns 0 on success (fence signaled), -ETIME on timeout,
 * -EINTR if interrupted by signal.
 */
int dma_fence_wait_timeout(struct dma_fence *fence, uint64_t timeout_ns);

/* ═══════════════════════════════════════════════════════════════════
 *  Callback registration
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * dma_fence_add_callback — Register a completion callback.
 *
 * If the fence is already signaled, the callback fires immediately
 * (synchronously).  Otherwise it is stored and fired when the fence
 * is signaled.
 *
 * @fence: The fence to observe.
 * @cb:    Callback function (called with fence and @data).
 * @data:  Opaque pointer passed to the callback.
 *
 * Returns 0 if callback was registered (or already fired),
 * negative errno on failure (-ENOSPC if too many callbacks).
 */
int dma_fence_add_callback(struct dma_fence *fence,
                           dma_fence_cb_t cb, void *data);

/**
 * dma_fence_remove_callback — Remove a previously registered callback.
 *
 * @fence: The fence.
 * @cb:    Callback function pointer.
 * @data:  Callback data pointer (both must match the registered pair).
 *
 * Returns 0 if removed, -ENOENT if not found.
 */
int dma_fence_remove_callback(struct dma_fence *fence,
                              dma_fence_cb_t cb, void *data);

#endif /* DRM_FENCE_H */
