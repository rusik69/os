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
            /* Allocate default-size buffer */
            pipe_table[i].buf = (uint8_t *)kmalloc(PIPE_DEFAULT_SIZE);
            if (!pipe_table[i].buf)
                return -1;
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

    /* Allocate new buffer */
    uint8_t *new_buf = (uint8_t *)kmalloc(new_capacity);
    if (!new_buf)
        return -1;

    /* Free old buffer, swap in new one */
    kfree(p->buf);
    p->buf = new_buf;
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
