#include "pipe.h"
#include "scheduler.h"
#include "string.h"
#include "signal.h"
#include "process.h"
#include "timer.h"
#include "syscall.h"
#include "heap.h"

static struct pipe pipe_table[PIPE_MAX];

void pipe_init(void) {
    memset(pipe_table, 0, sizeof(pipe_table));
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_table[i].in_use) {
            /* Allocate default-size buffers (primary + secondary for double-buffering) */
            pipe_table[i].buf = (uint8_t *)kmalloc(PIPE_DEFAULT_SIZE);
            if (!pipe_table[i].buf)
                return -1;
            pipe_table[i].buf2 = (uint8_t *)kmalloc(PIPE_DEFAULT_SIZE);
            if (!pipe_table[i].buf2) {
                kfree(pipe_table[i].buf);
                pipe_table[i].buf = NULL;
                return -1;
            }
            pipe_table[i].capacity  = PIPE_DEFAULT_SIZE;
            pipe_table[i].read_pos  = 0;
            pipe_table[i].write_pos = 0;
            pipe_table[i].count     = 0;
            pipe_table[i].readers   = 1;
            pipe_table[i].writers   = 1;
            pipe_table[i].in_use    = 1;
            pipe_table[i].flags     = 0;
            pipe_table[i].sigio_pid = 0;
            wait_queue_init(&pipe_table[i].read_wq);
            wait_queue_init(&pipe_table[i].write_wq);
            return i;
        }
    }
    return -1;
}

int pipe_write(int pipe_id, const void *buf, int len) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    if (len <= 0) return -1;

    struct pipe *p = &pipe_table[pipe_id];
    if (p->readers == 0) {
        struct process *cur = process_get_current();
        if (cur) signal_send(cur->pid, SIGPIPE);
        return -1;
    }

    const uint8_t *src = (const uint8_t *)buf;
    int written = 0;

    while (written < len) {
        while (p->count == p->capacity) {
            wait_queue_ensure(&p->write_wq);
            scheduler_yield();
            if (p->readers == 0) {
                struct process *cur = process_get_current();
                if (cur) signal_send(cur->pid, SIGPIPE);
                return written > 0 ? written : -1;
            }
        }

        int space = p->capacity - p->count;
        int chunk = (len - written) < space ? (len - written) : space;

        for (int i = 0; i < chunk; i++) {
            int pos = (p->write_pos + i) % p->capacity;
            p->buf[pos] = src[written + i];
        }

        p->write_pos = (p->write_pos + chunk) % p->capacity;
        p->count += chunk;
        written += chunk;

        if (p->count > 0) {
            wait_queue_wake(&p->read_wq);
        }
    }

    return written;
}

/* ── pipe_splice ────────────────────────────────────────────────────────
 * Splice data from src_pipe directly into dst_pipe without userspace
 * buffering.  Uses double-buffering for zero-copy transfers when possible.
 * Returns number of bytes moved, or -1 on error.
 */
int pipe_splice(int src_pipe_id, int dst_pipe_id, int len)
{
    if (src_pipe_id < 0 || src_pipe_id >= PIPE_MAX || !pipe_table[src_pipe_id].in_use)
        return -1;
    if (dst_pipe_id < 0 || dst_pipe_id >= PIPE_MAX || !pipe_table[dst_pipe_id].in_use)
        return -1;

    struct pipe *src = &pipe_table[src_pipe_id];
    struct pipe *dst = &pipe_table[dst_pipe_id];
    int total = 0;

    while (total < len) {
        /* Wait for source data */
        while (src->count == 0) {
            if (src->writers == 0) return total; /* EOF */
            wait_queue_ensure(&src->read_wq);
            scheduler_yield();
            if (src->readers == 0) return total > 0 ? total : -1;
        }

        /* Wait for destination space */
        while (dst->count == dst->capacity) {
            if (dst->readers == 0) {
                struct process *cur = process_get_current();
                if (cur) signal_send(cur->pid, SIGPIPE);
                return total > 0 ? total : -1;
            }
            wait_queue_ensure(&dst->write_wq);
            scheduler_yield();
        }

        /* Calculate how much we can move in one shot */
        int src_avail = src->count;
        int dst_space = dst->capacity - dst->count;
        int chunk = len - total;
        if (chunk > src_avail) chunk = src_avail;
        if (chunk > dst_space) chunk = dst_space;
        if (chunk <= 0) break;

        /* Double-buffering splice: swap source buffer with secondary
         * buffer for zero-copy transfer when the source has enough data
         * to fill a whole chunk and the destination has enough space. */
        if ((size_t)chunk >= (size_t)(src->capacity / 2) &&
            dst->count == 0 && dst->buf2) {
            /* Swap src's primary buffer into dst, and give src dst's
             * secondary buffer so the writer can keep going. */
            uint8_t *tmp = dst->buf;
            dst->buf = src->buf;
            src->buf = tmp;

            dst->write_pos = chunk;
            dst->count     = chunk;
            src->read_pos  = chunk;
            src->count    -= chunk;

            total   += chunk;
            /* Wake up waiters on both pipes */
            wait_queue_wake(&src->write_wq);
            wait_queue_wake(&dst->read_wq);
            continue;
        }

        /* Move data directly between pipe buffers */
        for (int i = 0; i < chunk; i++) {
            int src_pos = (src->read_pos + i) % src->capacity;
            int dst_pos = (dst->write_pos + i) % dst->capacity;
            dst->buf[dst_pos] = src->buf[src_pos];
        }

        src->read_pos = (src->read_pos + chunk) % src->capacity;
        src->count -= chunk;
        dst->write_pos = (dst->write_pos + chunk) % dst->capacity;
        dst->count += chunk;
        total += chunk;

        /* Wake up waiters on both pipes */
        wait_queue_wake(&src->write_wq);
        wait_queue_wake(&dst->read_wq);
    }

    return total;
}

int pipe_read(int pipe_id, void *buf, int len) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    if (len <= 0) return -1;

    struct pipe *p = &pipe_table[pipe_id];
    uint8_t *dst = (uint8_t *)buf;
    int total = 0;

    while (total < len) {
        while (p->count == 0) {
            if (p->writers == 0) {
                return total;  /* EOF */
            }
            wait_queue_ensure(&p->read_wq);
            scheduler_yield();
            if (p->writers == 0) {
                return total;
            }
        }

        int avail = p->count;
        int chunk = (len - total) < avail ? (len - total) : avail;

        for (int i = 0; i < chunk; i++) {
            int pos = (p->read_pos + i) % p->capacity;
            dst[total + i] = p->buf[pos];
        }

        p->read_pos = (p->read_pos + chunk) % p->capacity;
        p->count -= chunk;
        total += chunk;

        if (p->count < p->capacity) {
            wait_queue_wake(&p->write_wq);
        }
    }

    return total;
}

int pipe_close(int pipe_id, int is_write_end) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;

    struct pipe *p = &pipe_table[pipe_id];

    if (is_write_end) {
        p->writers--;
        if (p->writers == 0) {
            wait_queue_wake(&p->read_wq);  /* wake blocking readers */
        }
    } else {
        p->readers--;
        if (p->readers == 0) {
            wait_queue_wake(&p->write_wq);  /* wake blocking writers */
        }
    }

    /* Free pipe when both ends closed */
    if (p->readers == 0 && p->writers == 0) {
        if (p->buf) {
            kfree(p->buf);
            p->buf = NULL;
        }
        if (p->buf2) {
            kfree(p->buf2);
            p->buf2 = NULL;
        }
        memset(p, 0, sizeof(*p));
    }

    return 0;
}

int pipe_set_capacity(int pipe_id, int new_capacity) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;

    struct pipe *p = &pipe_table[pipe_id];

    /* Must be at least PIPE_BUF_SIZE and at most PIPE_MAX_SIZE */
    if (new_capacity < PIPE_BUF_SIZE || new_capacity > PIPE_MAX_SIZE)
        return -1;

    /* Only allowed when pipe is empty */
    if (p->count != 0)
        return -1;

    /* Allocate new buffers */
    uint8_t *new_buf = (uint8_t *)kmalloc(new_capacity);
    if (!new_buf)
        return -1;
    uint8_t *new_buf2 = (uint8_t *)kmalloc(new_capacity);
    if (!new_buf2) {
        kfree(new_buf);
        return -1;
    }

    /* Free old buffers, swap in new ones */
    kfree(p->buf);
    if (p->buf2)
        kfree(p->buf2);
    p->buf = new_buf;
    p->buf2 = new_buf2;
    p->capacity = new_capacity;
    p->read_pos = 0;
    p->write_pos = 0;

    return new_capacity;
}

int pipe_get_capacity(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    return pipe_table[pipe_id].capacity;
}

/* ── poll/select support ─────────────────────────────────────────── */
int pipe_poll(int pipe_id, int is_read_end) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return 0;
    struct pipe *p = &pipe_table[pipe_id];
    if (is_read_end)
        return p->count > 0 ? (POLLIN) : 0;
    else
        return p->count < p->capacity ? (POLLOUT) : 0;
}

/* ── fcntl F_SETFL — O_NONBLOCK ──────────────────────────────────── */
int pipe_set_nonblock(int pipe_id, int nonblock) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    struct pipe *p = &pipe_table[pipe_id];
    if (nonblock)
        p->flags |= PIPE_FLAG_NONBLOCK;
    else
        p->flags &= ~PIPE_FLAG_NONBLOCK;
    return 0;
}

/* ── fcntl F_SETOWN / F_SETSIG — SIGIO owner ─────────────────────── */
int pipe_set_sigio(int pipe_id, uint32_t pid) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;

    pipe_table[pipe_id].sigio_pid = pid;
    return 0;
}

/* ── pipe_available — bytes available for reading ─────────────────── */
int pipe_available(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    return pipe_table[pipe_id].count;
}
/* ── FIFO unlink — close one end ─────────────────────────────────── */
void pipe_close_read(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (!p->in_use) return;
    p->readers--;
    if (p->readers <= 0 || p->writers <= 0) {
        if (p->readers == 0) wait_queue_wake(&p->write_wq);
        if (p->writers == 0) wait_queue_wake(&p->read_wq);
    }
}

void pipe_close_write(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (!p->in_use) return;
    p->writers--;
    if (p->readers <= 0 || p->writers <= 0) {
        if (p->readers == 0) wait_queue_wake(&p->write_wq);
        if (p->writers == 0) wait_queue_wake(&p->read_wq);
    }
}

/* ── Wait queue lazy init ────────────────────────────────────────── */
/* Called from pipe tight loops as a safety measure; lock init is cheap. */
void wait_queue_ensure(struct wait_queue *wq) {
    if (wq) spinlock_init(&wq->lock);
}
