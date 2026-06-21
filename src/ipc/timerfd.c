#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "timer.h"
#include "waitqueue.h"
#include "signal.h"
#include "errno.h"
#include "socket.h"

/* ── timerfd: create file descriptors that fire timer events ─────────── */

#define TIMERFD_MAX 16

/* Timerfd settime flags (matching Linux TFD_TIMER_ABSTIME) */
#define TFD_TIMER_ABSTIME (1 << 0)

struct itimerspec {
    struct timespec it_interval; /* timer period (0 = one-shot) */
    struct timespec it_value;    /* initial expiration */
};

struct timerfd_entry {
    int in_use;
    uint32_t pid;
    uint64_t expiration_tick;   /* absolute tick when timer fires */
    uint64_t interval_ticks;    /* 0 = one-shot, >0 = periodic */
    int      absolute;           /* 1 = it_value was absolute (TFD_TIMER_ABSTIME) */
    int      expired;           /* 1 = has data to read */
    uint64_t expirations;       /* number of expirations since last read */
    struct wait_queue wq;
};

static struct timerfd_entry timerfd_table[TIMERFD_MAX];
static int timerfd_initialized = 0;

void timerfd_init(void) {
    if (timerfd_initialized) return;
    memset(timerfd_table, 0, sizeof(timerfd_table));
    timerfd_initialized = 1;
    kprintf("[OK] timerfd initialized\n");
}

/* Convert timespec to ticks (assuming 100Hz timer) */
static uint64_t timespec_to_ticks(struct timespec *ts) {
    return ts->tv_sec * 100 + ts->tv_nsec / 10000000; /* 10ms per tick */
}

/* Create timerfd */
int timerfd_create(int clockid) {
    (void)clockid;
    if (!timerfd_initialized) return -1;
    
    struct process *cur = process_get_current();
    if (!cur) return -1;
    
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i].in_use) {
            timerfd_table[i].in_use = 1;
            timerfd_table[i].pid = cur->pid;
            timerfd_table[i].expiration_tick = 0;
            timerfd_table[i].interval_ticks = 0;
            timerfd_table[i].absolute = 0;
            timerfd_table[i].expired = 0;
            timerfd_table[i].expirations = 0;
            wait_queue_init(&timerfd_table[i].wq);
            return 300 + i; /* fd base */
        }
    }
    return -ENFILE;
}

/* Set timerfd */
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    int slot = fd - 300;
    if (slot < 0 || slot >= TIMERFD_MAX) return -EBADF;
    if (!timerfd_table[slot].in_use) return -EBADF;
    
    struct timerfd_entry *tf = &timerfd_table[slot];
    
    /* Save old value if requested */
    if (old_value) {
        if (tf->expiration_tick == 0) {
            /* Timer was disarmed */
            old_value->it_value.tv_sec = 0;
            old_value->it_value.tv_nsec = 0;
        } else if (tf->absolute) {
            /* Absolute timer: return the remaining absolute time */
            uint64_t ticks_left = tf->expiration_tick;
            if (ticks_left > timer_get_ticks()) {
                ticks_left = tf->expiration_tick - timer_get_ticks();
            } else {
                ticks_left = 0; /* already expired */
            }
            old_value->it_value.tv_sec = ticks_left / 100;
            old_value->it_value.tv_nsec = (ticks_left % 100) * 10000000;
        } else {
            /* Relative timer: return the remaining time as a relative value */
            uint64_t ticks_left = 0;
            if (tf->expiration_tick > timer_get_ticks()) {
                ticks_left = tf->expiration_tick - timer_get_ticks();
            }
            old_value->it_value.tv_sec = ticks_left / 100;
            old_value->it_value.tv_nsec = (ticks_left % 100) * 10000000;
        }
        old_value->it_interval.tv_sec = tf->interval_ticks / 100;
        old_value->it_interval.tv_nsec = (tf->interval_ticks % 100) * 10000000;
    }
    
    if (!new_value ||
        (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0)) {
        /* Disarm */
        tf->expiration_tick = 0;
        tf->interval_ticks = 0;
        tf->expired = 0;
        tf->expirations = 0;
        return 0;
    }
    
    uint64_t now = timer_get_ticks();
    uint64_t ticks = timespec_to_ticks((struct timespec *)&new_value->it_value);
    
    tf->absolute = (flags & TFD_TIMER_ABSTIME) ? 1 : 0;
    
    if (tf->absolute) {
        /* Absolute time: it_value is in the same time base as timer_get_ticks()
         * (monotonic time since boot).  Convert the timespec directly to ticks. */
        tf->expiration_tick = ticks;
    } else {
        /* Relative time: it_value is added to the current tick count */
        tf->expiration_tick = now + ticks;
    }
    
    tf->interval_ticks = timespec_to_ticks((struct timespec *)&new_value->it_interval);
    tf->expired = 0;
    tf->expirations = 0;
    
    return 0;
}

/* Get timerfd settings */
int timerfd_gettime(int fd, struct itimerspec *cur_value) {
    int slot = fd - 300;
    if (slot < 0 || slot >= TIMERFD_MAX) return -EBADF;
    if (!timerfd_table[slot].in_use) return -EBADF;
    
    struct timerfd_entry *tf = &timerfd_table[slot];
    
    if (cur_value) {
        uint64_t ticks_left = 0;
        if (tf->expiration_tick > timer_get_ticks()) {
            ticks_left = tf->expiration_tick - timer_get_ticks();
        }
        cur_value->it_value.tv_sec = ticks_left / 100;
        cur_value->it_value.tv_nsec = (ticks_left % 100) * 10000000;
        cur_value->it_interval.tv_sec = tf->interval_ticks / 100;
        cur_value->it_interval.tv_nsec = (tf->interval_ticks % 100) * 10000000;
    }
    
    return 0;
}

/* Read expirations from timerfd */
uint64_t timerfd_read(int fd) {
    int slot = fd - 300;
    if (slot < 0 || slot >= TIMERFD_MAX) return 0;
    if (!timerfd_table[slot].in_use) return 0;
    
    struct timerfd_entry *tf = &timerfd_table[slot];
    
    /* Wait for expiration */
    while (!tf->expired) {
        wait_queue_sleep(&tf->wq);
    }
    
    uint64_t count = tf->expirations;
    tf->expirations = 0;
    tf->expired = 0;
    
    /* Re-arm if periodic */
    if (tf->interval_ticks > 0) {
        /* For absolute timers, preserve alignment by adding to the original
         * expiration base.  For relative timers, the same approach avoids
         * accumulating drift from late handling. */
        tf->expiration_tick += tf->interval_ticks;
        uint64_t now = timer_get_ticks();
        /* If we've fallen behind, catch up by skipping missed expirations */
        while (tf->expiration_tick <= now) {
            tf->expiration_tick += tf->interval_ticks;
        }
    }
    
    return count;
}

/* Check and trigger timerfd expirations (called from timer tick) */
void timerfd_tick(void) {
    if (!timerfd_initialized) return;
    uint64_t now = timer_get_ticks();
    
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i].in_use) continue;
        if (timerfd_table[i].expiration_tick == 0) continue;
        
        if (now >= timerfd_table[i].expiration_tick) {
            timerfd_table[i].expired = 1;
            timerfd_table[i].expirations++;
            wait_queue_wake(&timerfd_table[i].wq);
            
            if (timerfd_table[i].interval_ticks > 0) {
                /* Periodic: advance by the interval, skipping missed expirations.
                 * Using += preserves timing alignment and avoids drift. */
                timerfd_table[i].expiration_tick += timerfd_table[i].interval_ticks;
                /* If we've fallen far behind, skip ahead to catch up */
                while (timerfd_table[i].expiration_tick <= now) {
                    timerfd_table[i].expiration_tick += timerfd_table[i].interval_ticks;
                }
            } else {
                timerfd_table[i].expiration_tick = 0; /* one-shot */
            }
        }
    }
}

/* ── timerfd_poll ─────────────────────────────────────── */
int timerfd_poll(int fd)
{
    int slot = fd - 300;
    if (slot < 0 || slot >= TIMERFD_MAX) return POLLNVAL;
    if (!timerfd_table[slot].in_use) return POLLNVAL;

    struct timerfd_entry *tf = &timerfd_table[slot];
    int mask = POLLOUT; /* always writable */

    if (tf->expired)
        mask |= POLLIN;

    return mask;
}

/* ── timerfd_show_fdinfo ──────────────────────────────── */
int timerfd_show_fdinfo(int fd, char *buf, size_t size)
{
    if (!buf || size == 0) return -EINVAL;

    int slot = fd - 300;
    if (slot < 0 || slot >= TIMERFD_MAX || !timerfd_table[slot].in_use) {
        int n = snprintf(buf, size, "timerfd:\t%d (invalid)\n", fd);
        return n < 0 ? -EINVAL : n;
    }

    struct timerfd_entry *tf = &timerfd_table[slot];
    int n = snprintf(buf, size,
        "timerfd:\t%d\n"
        "pid:\t%u\n"
        "expiration_tick:\t%llu\n"
        "interval_ticks:\t%llu\n"
        "expired:\t%d\n"
        "expirations:\t%llu\n",
        fd, tf->pid,
        (unsigned long long)tf->expiration_tick,
        (unsigned long long)tf->interval_ticks,
        tf->expired,
        (unsigned long long)tf->expirations);

    if (n < 0) return -EINVAL;
    if ((size_t)n >= size) return -ENOSPC;
    return n;
}