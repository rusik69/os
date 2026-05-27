#include "pipe.h"
#include "scheduler.h"
#include "string.h"
#include "signal.h"
#include "process.h"
#include "timer.h"

#define PIPE_WAITERS 4

static struct pipe pipe_table[PIPE_MAX];

static void pipe_add_waiter(uint32_t *waiters, uint32_t pid) {
    for (int i = 0; i < PIPE_WAITERS; i++) {
        if (waiters[i] == 0) { waiters[i] = pid; return; }
    }
}

static void pipe_wake_waiters(uint32_t *waiters) {
    for (int i = 0; i < PIPE_WAITERS; i++) {
        if (waiters[i]) {
            uint32_t pid = waiters[i];
            waiters[i] = 0;
            struct process *p = process_get_by_pid(pid);
            if (p && p->state == PROCESS_BLOCKED) {
                p->state = PROCESS_READY;
                p->last_run_tick = timer_get_ticks();
                scheduler_add(p);
            }
        }
    }
}

static int pipe_is_self_deadlock(uint32_t *waiters, uint32_t pid) {
    for (int i = 0; i < PIPE_WAITERS; i++) {
        if (waiters[i] == pid) return 1;
    }
    return 0;
}

void pipe_init(void) {
    memset(pipe_table, 0, sizeof(pipe_table));
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_table[i].in_use) {
            pipe_table[i].read_pos  = 0;
            pipe_table[i].write_pos = 0;
            pipe_table[i].count     = 0;
            pipe_table[i].readers   = 1;
            pipe_table[i].writers   = 1;
            pipe_table[i].in_use    = 1;
            memset(pipe_table[i].blocked_read_pids, 0, sizeof(pipe_table[i].blocked_read_pids));
            memset(pipe_table[i].blocked_write_pids, 0, sizeof(pipe_table[i].blocked_write_pids));
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
        while (p->count == PIPE_BUF_SIZE) {
            if (p->readers == 0) return written ? written : -1;
            struct process *cur = process_get_current();
            /* Detect self-deadlock: same process is the only reader */
            if (cur && pipe_is_self_deadlock(p->blocked_read_pids, cur->pid))
                return written ? written : -1;
            if (cur && cur->state == PROCESS_RUNNING) {
                pipe_add_waiter(p->blocked_write_pids, cur->pid);
                cur->state = PROCESS_BLOCKED;
                scheduler_remove(cur);
                scheduler_yield();
            } else {
                return written ? written : -1;
            }
        }

        int space = PIPE_BUF_SIZE - p->count;
        int to_write = len - written;
        if (to_write > space) to_write = space;

        for (int i = 0; i < to_write; i++) {
            p->buf[p->write_pos] = src[written + i];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        }
        p->count  += to_write;
        written   += to_write;
        pipe_wake_waiters(p->blocked_read_pids);
    }
    return written;
}

int pipe_read(int pipe_id, void *buf, int len) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;
    if (len <= 0) return -1;

    struct pipe *p = &pipe_table[pipe_id];

    while (p->count == 0) {
        if (p->writers == 0) return 0;
        struct process *cur = process_get_current();
        if (cur && cur->state == PROCESS_RUNNING) {
            pipe_add_waiter(p->blocked_read_pids, cur->pid);
            cur->state = PROCESS_BLOCKED;
            scheduler_remove(cur);
            scheduler_yield();
        } else {
            return 0;
        }
    }

    uint8_t *dst = (uint8_t *)buf;
    int to_read = (p->count < len) ? p->count : len;

    for (int i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }
    p->count -= to_read;
    pipe_wake_waiters(p->blocked_write_pids);
    return to_read;
}

void pipe_close_read(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (p->readers > 0) p->readers--;
    pipe_wake_waiters(p->blocked_write_pids);
    if (p->readers == 0 && p->writers == 0) p->in_use = 0;
}

void pipe_close_write(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (p->writers > 0) p->writers--;
    pipe_wake_waiters(p->blocked_read_pids);
    if (p->readers == 0 && p->writers == 0) p->in_use = 0;
}

int pipe_available(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return 0;
    return pipe_table[pipe_id].count;
}
