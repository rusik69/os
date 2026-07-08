#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "scheduler.h"
#include "vfs.h"
#include "heap.h"
#include "signal.h"
#include "pmm.h"
#include "workqueue.h"
#include "timer.h"

/* ── Async I/O (AIO) basic implementation ────────────────────────────── */

#define AIO_MAX_CTX    4   /* max AIO context slots */
#define AIO_MAX_EVENTS 64
#define AIO_MAX_IO 256

/* AIO control block */
struct aiocb {
    int      aio_fildes;
    uint64_t aio_buf;
    uint64_t aio_nbytes;
    uint64_t aio_offset;
    int      aio_reqprio;
    int      aio_lio_opcode;
    int      aio_state;  /* 0=ready, 1=in-progress, 2=done, 3=error */
    int      aio_errno;
    uint64_t aio_return;
    int      in_use;
    uint32_t pid; /* owner PID */
};

/* Event for aio_return/suspend */
struct aio_event {
    uint64_t obj;    /* aiocb pointer */
    uint64_t data;   /* user data */
    uint32_t pid;    /* owner PID */
    int      signo;  /* signal on completion */
    int      in_use;
};

static struct aiocb aio_cbs[AIO_MAX_IO];
static struct aio_event aio_events[AIO_MAX_EVENTS];
static int aio_initialized = 0;

static void aio_ext_init(void) {
    if (aio_initialized) return;
    memset(aio_cbs, 0, sizeof(aio_cbs));
    memset(aio_events, 0, sizeof(aio_events));
    aio_initialized = 1;
    kprintf("[OK] AIO extended initialized (%d slots)\n", AIO_MAX_IO);
}

/* Resolve fd to path */
static const char *aio_fd_to_path(int fd) {
    struct process *p = process_get_current();
    if (!p) return NULL;
    int i = fd - 3;
    if (i < 0 || i >= PROCESS_FD_MAX) return NULL;
    if (!p->fd_table[i].used) return NULL;
    return p->fd_table[i].path;
}

/* Submit an AIO read request */
static int aio_read_submit(struct aiocb *cb) {
    if (!cb) return -EINVAL;

    const char *path = aio_fd_to_path(cb->aio_fildes);
    if (!path) return -EBADF;

    /* Allocate a temp buffer */
    void *buf = kmalloc(cb->aio_nbytes);
    if (!buf) return -ENOMEM;

    /* Perform the read via VFS */
    uint32_t out_size = 0;
    int ret = vfs_read(path, buf, cb->aio_nbytes, &out_size);
    if (ret < 0) {
        kfree(buf);
        cb->aio_state = 3; /* error */
        cb->aio_errno = -ret;
        return ret;
    }

    /* Copy to user buffer (simplified: kernel buffer) */
    /* In real implementation, would copy to user-space via copy_to_user */
    cb->aio_return = out_size;
    cb->aio_state = 2; /* done */
    kfree(buf);
    return 0;
}

/* Submit an AIO write request */
static int aio_write_submit(struct aiocb *cb) {
    if (!cb) return -EINVAL;

    const char *path = aio_fd_to_path(cb->aio_fildes);
    if (!path) return -EBADF;

    void *buf = kmalloc(cb->aio_nbytes);
    if (!buf) return -ENOMEM;

    /* In real impl, copy from userspace */
    memset(buf, 0, cb->aio_nbytes); /* stub data */

    int ret = vfs_write(path, buf, cb->aio_nbytes);
    if (ret < 0) {
        kfree(buf);
        cb->aio_state = 3;
        cb->aio_errno = -ret;
        return ret;
    }

    cb->aio_return = cb->aio_nbytes;
    cb->aio_state = 2;
    kfree(buf);
    return 0;
}

/* ── Workqueue-based background AIO processing ─────────────────────── */

/** Work item wrapping an AIO control block index for background execution */
struct aio_work {
    int idx;
};

static void aio_work_handler(void *arg)
{
    struct aio_work *work = (struct aio_work *)arg;
    int idx = work->idx;
    kfree(work);

    if (idx < 0 || idx >= AIO_MAX_IO || !aio_cbs[idx].in_use)
        return;

    struct aiocb *cb = &aio_cbs[idx];

    kprintf("[AIO] Processing request #%d in background (opcode=%d)\n",
            idx, cb->aio_lio_opcode);

    int ret;
    if (cb->aio_lio_opcode == 0) { /* read */
        ret = aio_read_submit(cb);
    } else if (cb->aio_lio_opcode == 1) { /* write */
        ret = aio_write_submit(cb);
    } else {
        cb->aio_state = 3;
        cb->aio_errno = EINVAL;
        ret = -EINVAL;
    }

    if (ret < 0 && cb->aio_state == 1) {
        cb->aio_state = 3;
        cb->aio_errno = -ret;
    }
}

/* Submit aio request */
static int aio_submit(struct aiocb *user_cb) {
    if (!aio_initialized || !user_cb) return -EINVAL;

    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < AIO_MAX_IO; i++) {
        if (!aio_cbs[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -EAGAIN;

    /* Copy control block */
    memcpy(&aio_cbs[idx], user_cb, sizeof(struct aiocb));
    aio_cbs[idx].in_use = 1;
    aio_cbs[idx].aio_state = 1; /* in progress */
    struct process *cur = process_get_current();
    aio_cbs[idx].pid = cur ? cur->pid : 0;

    /* Schedule AIO request for background processing via workqueue */
    struct aio_work *work = (struct aio_work *)kmalloc(sizeof(struct aio_work));
    if (!work) {
        aio_cbs[idx].in_use = 0;
        return -ENOMEM;
    }
    work->idx = idx;

    int ret = workqueue_schedule(aio_work_handler, work);
    if (ret < 0) {
        kfree(work);
        aio_cbs[idx].in_use = 0;
        return ret;
    }

    kprintf("[AIO] Request #%d queued in background\n", idx);

    return idx;
}

/* Get return value of completed AIO request */
static int aio_return(uint64_t aiocb_ptr) {
    (void)aiocb_ptr;
    return 0;
}

/* Suspend until AIO completion */
static int aio_suspend(uint64_t *aiocb_list, int nent, uint64_t timeout) {
    (void)aiocb_list;
    (void)nent;
    (void)timeout;
    return 0;
}

/* Cancel an AIO request */
static int aio_cancel(int fd, uint64_t aiocb_ptr) {
    (void)fd;
    (void)aiocb_ptr;
    for (int i = 0; i < AIO_MAX_IO; i++) {
        if (aio_cbs[i].in_use) {
            aio_cbs[i].in_use = 0;
            break;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete AIO operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── aio_cancel_all: Cancel all pending AIO requests for a given fd ───── */
static int aio_cancel_all(int fd)
{
    if (!aio_initialized) return -EINVAL;

    kprintf("[AIO] aio_cancel_all: cancelling all requests for fd %d\n", fd);
    int count = 0;
    for (int i = 0; i < AIO_MAX_IO; i++) {
        if (aio_cbs[i].in_use) {
            const char *path = aio_fd_to_path(fd);
            if (!path) continue;
            aio_cbs[i].in_use = 0;
            aio_cbs[i].aio_state = 3; /* cancelled */
            count++;
        }
    }
    kprintf("[AIO] aio_cancel_all: cancelled %d requests\n", count);
    return count;
}

/* ── aio_get_events: Collect completed AIO events ──────────────────────── */
static int aio_get_events(uint64_t timeout_ms, struct aio_event *events, int max_events)
{
    if (!aio_initialized || !events || max_events <= 0) return -EINVAL;

    int collected = 0;
    for (int i = 0; i < AIO_MAX_EVENTS && collected < max_events; i++) {
        if (aio_events[i].in_use) {
            memcpy(&events[collected], &aio_events[i], sizeof(struct aio_event));
            aio_events[i].in_use = 0;
            collected++;
        }
    }

    if (timeout_ms > 0 && collected == 0) {
        /* Simple spin-wait for completion (in real impl would use timers) */
        uint64_t start = timer_get_ticks();
        uint64_t timeout_ticks = timeout_ms * TIMER_FREQ / 1000;
        while (collected == 0 && (timer_get_ticks() - start) < timeout_ticks) {
            for (int i = 0; i < AIO_MAX_EVENTS && collected < max_events; i++) {
                if (aio_events[i].in_use) {
                    memcpy(&events[collected], &aio_events[i], sizeof(struct aio_event));
                    aio_events[i].in_use = 0;
                    collected++;
                }
            }
            if (collected == 0) scheduler_yield();
        }
    }

    return collected;
}

/* ── aio_fsync: Sync AIO data ──────────────────────────────────────────── */
int aio_fsync(int fd, int op)
{
    (void)fd;
    (void)op;
    /* In a minimal implementation, all data is already committed through VFS */
    kprintf("[AIO] aio_fsync: fsync on fd %d (op=%d)\n", fd, op);
    return 0;
}
