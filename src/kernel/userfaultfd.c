#include "userfaultfd.h"
#include "string.h"
#include "kernel.h"
#include "heap.h"
#include "printf.h"
#include "errno.h"

static struct uffd_context uffd_table[UFFD_MAX_CONTEXTS];
static int uffd_initialised = 0;

void uffd_init(void)
{
    if (uffd_initialised)
        return;

    for (int i = 0; i < UFFD_MAX_CONTEXTS; i++) {
        struct uffd_context *ctx = &uffd_table[i];
        spinlock_init(&ctx->lock);
        ctx->used = 0;
        ctx->fd = i;
        ctx->event_head = 0;
        ctx->event_tail = 0;
        memset(ctx->events, 0, sizeof(ctx->events));
    }
    uffd_initialised = 1;
    kprintf("uffd: initialised with %d contexts\n", UFFD_MAX_CONTEXTS);
}

static int uffd_find_free(void)
{
    for (int i = 0; i < UFFD_MAX_CONTEXTS; i++) {
        if (!uffd_table[i].used)
            return i;
    }
    return -EMFILE;
}

int userfaultfd_create(int flags)
{
    (void)flags;

    if (!uffd_initialised)
        return -ENOSYS;

    int fd = uffd_find_free();
    if (fd < 0)
        return fd;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);
    ctx->used = 1;
    ctx->event_head = 0;
    ctx->event_tail = 0;
    spinlock_release(&ctx->lock);

    return fd;
}

int userfaultfd_register(int fd, uint64_t addr, uint64_t len, int mode)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (addr + len < addr || len == 0)
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);
    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }
    /* The userfaultfd implementation stores registered ranges on a per-context
     * basis.  For this stub we simply validate and record the mode; in a full
     * implementation the ranges would be kept in an interval tree. */
    (void)addr;
    (void)len;
    (void)mode;
    spinlock_release(&ctx->lock);
    return 0;
}

int userfaultfd_unregister(int fd, uint64_t addr, uint64_t len)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (addr + len < addr || len == 0)
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);
    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }
    (void)addr;
    (void)len;
    spinlock_release(&ctx->lock);
    return 0;
}

int userfaultfd_handle_fault(int fd, uint64_t fault_addr, int write)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    /* Check if the ring buffer is full. */
    int next_head = (ctx->event_head + 1) % UFFD_EVENT_RING_SIZE;
    if (next_head == ctx->event_tail) {
        spinlock_release(&ctx->lock);
        return -ENOSPC;  /* ring full */
    }

    struct uffd_event *ev = &ctx->events[ctx->event_head];
    ev->fault_addr = fault_addr;
    ev->fault_flags = write ? 1 : 0;
    ev->pending = 1;
    ctx->event_head = next_head;

    spinlock_release(&ctx->lock);
    return 0;
}

int64_t userfaultfd_read(int fd, void *buf, uint64_t count)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (!buf || count < sizeof(struct uffd_event))
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    if (ctx->event_tail == ctx->event_head) {
        spinlock_release(&ctx->lock);
        return 0;  /* no events available */
    }

    /* Dequeue a single event. */
    struct uffd_event *ev = &ctx->events[ctx->event_tail];
    if (count < sizeof(*ev)) {
        spinlock_release(&ctx->lock);
        return -EINVAL;
    }

    memcpy(buf, ev, sizeof(*ev));
    ev->pending = 0;
    ctx->event_tail = (ctx->event_tail + 1) % UFFD_EVENT_RING_SIZE;

    spinlock_release(&ctx->lock);
    return (int64_t)sizeof(struct uffd_event);
}
