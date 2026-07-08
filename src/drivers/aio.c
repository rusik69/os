#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "aio.h"
#include "spinlock.h"
#include "string.h"
#define MAX_AIO 64
static struct aio_request aio_queue[MAX_AIO];
static spinlock_t aio_lock;
void aio_init(void) {
    spinlock_init(&aio_lock);
    memset(aio_queue, 0, sizeof(aio_queue));
    kprintf("[OK] AIO subsystem initialized\n");
}
ssize_t aio_read(int fd, void *buf, size_t count, uint64_t offset) {
    if (!buf) return -1;
    kprintf("[aio] read fd=%d count=%llu offset=%llu\n", fd, (unsigned long long)count, (unsigned long long)offset);
    return (ssize_t)count;
}
ssize_t aio_write(int fd, const void *buf, size_t count, uint64_t offset) {
    if (!buf) return -1;
    kprintf("[aio] write fd=%d count=%llu offset=%llu\n", fd, (unsigned long long)count, (unsigned long long)offset);
    return (ssize_t)count;
}

/* ── Stub: aio_fsync ─────────────────────────────── */
int aio_fsync(int fd)
{
    (void)fd;
    kprintf("[aio] aio_fsync: not yet implemented\n");
    return 0;
}
/* ── Stub: aio_poll ─────────────────────────────── */
static int aio_poll(int fd, int events)
{
    (void)fd;
    (void)events;
    kprintf("[aio] aio_poll: not yet implemented\n");
    return 0;
}
