#include "aio_enhanced.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "types.h"
#include "spinlock.h"
#include "kernel.h"

/* AIO enhanced - supplementary async I/O operations.
 * Builds on drivers/aio.c with additional poll/getevents wrappers.
 */

#define AIO_MAX_CTX 4
#define AIO_RING_SIZE 64

struct aio_ctx_ext {
    int in_use;
    uint64_t ctx_id;
    struct io_event completion_ring[AIO_RING_SIZE];
    int head, tail;
    spinlock_t lock;
};

static struct aio_ctx_ext aio_ctx_ext_table[AIO_MAX_CTX];
static int aio_ext_initialised = 0;

static int ring_push(struct aio_ctx_ext *ctx, const struct io_event *evt) {
    int next = (ctx->head + 1) % AIO_RING_SIZE;
    if (next == ctx->tail) return -EAGAIN;
    ctx->completion_ring[ctx->head] = *evt;
    ctx->head = next;
    return 0;
}

static int ring_pop(struct aio_ctx_ext *ctx, struct io_event *evt) {
    if (ctx->tail == ctx->head) return -EAGAIN;
    *evt = ctx->completion_ring[ctx->tail];
    ctx->tail = (ctx->tail + 1) % AIO_RING_SIZE;
    return 0;
}

int aio_ext_poll(int fd, uint64_t events) {
    if (!aio_ext_initialised) return -ENOSYS;
    if (fd < 0) return -EBADF;

    struct io_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.data = events;
    evt.obj = (uint64_t)(uintptr_t)fd;
    evt.res = events;  /* simplified: all events "ready" */

    struct aio_ctx_ext *ctx = &aio_ctx_ext_table[0];
    spinlock_acquire(&ctx->lock);
    int ret = ring_push(ctx, &evt);
    spinlock_release(&ctx->lock);
    return ret;
}

int aio_ext_getevents(uint64_t ctx_id, long min_nr, long max_nr,
                      struct io_event *events, struct timespec *timeout) {
    (void)ctx_id;
    (void)min_nr;
    (void)timeout;
    if (!events || !aio_ext_initialised) return -EINVAL;
    if (max_nr <= 0) return -EINVAL;

    struct aio_ctx_ext *ctx = &aio_ctx_ext_table[0];
    int count = 0;

    spinlock_acquire(&ctx->lock);
    while (count < max_nr && ring_pop(ctx, &events[count]) == 0) {
        count++;
    }
    spinlock_release(&ctx->lock);
    return count;
}

int aio_ext_setup(int nr_events, struct aio_context_ext *ctx_out) {
    (void)nr_events;
    if (!ctx_out) return -EINVAL;
    if (!aio_ext_initialised) return -ENOSYS;

    for (int i = 0; i < AIO_MAX_CTX; i++) {
        if (!aio_ctx_ext_table[i].in_use) {
            aio_ctx_ext_table[i].in_use = 1;
            aio_ctx_ext_table[i].head = 0;
            aio_ctx_ext_table[i].tail = 0;
            ctx_out->ctx_id = (uint64_t)i;
            return 0;
        }
    }
    return -ENOSPC;
}

int aio_ext_destroy(uint64_t ctx_id) {
    int idx = (int)ctx_id;
    if (idx < 0 || idx >= AIO_MAX_CTX) return -EINVAL;
    if (!aio_ctx_ext_table[idx].in_use) return -EINVAL;
    aio_ctx_ext_table[idx].in_use = 0;
    return 0;
}

void aio_enhanced_init(void) {
    for (int i = 0; i < AIO_MAX_CTX; i++) {
        aio_ctx_ext_table[i].in_use = 0;
        aio_ctx_ext_table[i].head = 0;
        aio_ctx_ext_table[i].tail = 0;
        spinlock_init(&aio_ctx_ext_table[i].lock);
    }
    aio_ext_initialised = 1;
    kprintf("[OK] AIO enhanced initialized (%d contexts)\n", AIO_MAX_CTX);
}
