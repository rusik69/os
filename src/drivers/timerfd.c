/*
 * timerfd.c — Timer file descriptor
 *
 * Implements timerfd_create, timerfd_settime, timerfd_gettime.
 * Supports TFD_TIMER_ABSTIME for absolute timer expiration.
 *
 * Timer fds range: 800..815 (max 16 timerfds)
 */
#define KERNEL_INTERNAL
#include "timerfd.h"
#include "types.h"
#include "timer.h"
#include "printf.h"
#include "string.h"
#include "scheduler.h"
#include "process.h"
#include "rtc.h"
#include "waitqueue.h"

#define TIMERFD_MAX 16
#define TIMERFD_BASE 800

struct timerfd_info {
    int      in_use;
    int      clockid;          /* CLOCK_REALTIME, CLOCK_MONOTONIC */
    int      flags;            /* TFD_NONBLOCK, TFD_CLOEXEC */
    /* Expiration settings */
    struct itimerspec settings;
    /* Next expiration in ticks (internal timer ticks) */
    uint64_t next_expiry_ticks;
    /* Interval in ticks (0 = one-shot) */
    uint64_t interval_ticks;
    /* Number of expirations that have occurred (read consumes this) */
    uint64_t expirations;
    /* Wait queue for readers blocking on timer */
    struct wait_queue wq;
};

static struct timerfd_info timerfd_table[TIMERFD_MAX];

/* Helper: convert timespec to ticks (for CLOCK_MONOTONIC relative) */
static uint64_t timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
    uint64_t total_ns = sec * 1000000000ULL + nsec;
    return total_ns / NS_PER_TICK;
}

/* Helper: get current time in ticks for a given clock id */
static uint64_t timerfd_current_ticks(int clockid)
{
    uint64_t now_ticks = timer_get_ticks();
    if (clockid == CLOCK_REALTIME) {
        /* CLOCK_REALTIME: convert epoch to ticks relative to boot */
        uint64_t epoch = rtc_get_epoch();
        uint64_t epoch_ticks = epoch * TIMER_FREQ;
        /* The wall clock time in ticks (approximate) */
        return epoch_ticks + now_ticks;
    }
    /* CLOCK_MONOTONIC, CLOCK_BOOTTIME */
    return now_ticks;
}

/* Allocate a timerfd slot */
static int timerfd_alloc(int clockid, int flags)
{
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i].in_use) {
            timerfd_table[i].in_use = 1;
            timerfd_table[i].clockid = clockid;
            timerfd_table[i].flags = flags;
            timerfd_table[i].next_expiry_ticks = 0;
            timerfd_table[i].interval_ticks = 0;
            timerfd_table[i].expirations = 0;
            memset(&timerfd_table[i].settings, 0, sizeof(struct itimerspec));
            wait_queue_init(&timerfd_table[i].wq);
            return TIMERFD_BASE + i;
        }
    }
    return -1;
}

static struct timerfd_info *timerfd_get(int fd)
{
    int slot = fd - TIMERFD_BASE;
    if (slot < 0 || slot >= TIMERFD_MAX) return NULL;
    if (!timerfd_table[slot].in_use) return NULL;
    return &timerfd_table[slot];
}

/* ── Public API ─────────────────────────────────────────────────────── */

void timerfd_init(void)
{
    memset(timerfd_table, 0, sizeof(timerfd_table));
    kprintf("[OK] timerfd subsystem initialized\n");
}

int timerfd_create(int clockid)
{
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
        clockid != CLOCK_BOOTTIME)
        return -1;
    return timerfd_alloc(clockid, 0);
}

int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value)
{
    struct timerfd_info *tfd = timerfd_get(fd);
    if (!tfd) return -1;

    /* Return old settings if requested */
    if (old_value)
        memcpy(old_value, &tfd->settings, sizeof(struct itimerspec));

    /* Store new settings */
    if (new_value) {
        memcpy(&tfd->settings, new_value, sizeof(struct itimerspec));

        uint64_t now_ticks = timerfd_current_ticks(tfd->clockid);

        if (new_value->it_value.tv_sec == 0 && new_value->it_value.tv_nsec == 0) {
            /* Disarm the timer */
            tfd->next_expiry_ticks = 0;
            tfd->interval_ticks = 0;
        } else {
            /* Calculate interval in ticks */
            tfd->interval_ticks = timespec_to_ticks(
                new_value->it_interval.tv_sec,
                new_value->it_interval.tv_nsec);

            if (flags & TFD_TIMER_ABSTIME) {
                /* Absolute time: interpret it_value as absolute clock time */
                uint64_t abs_ticks = timespec_to_ticks(
                    new_value->it_value.tv_sec,
                    new_value->it_value.tv_nsec);

                if (abs_ticks <= now_ticks) {
                    /* Already in the past — fire immediately */
                    tfd->next_expiry_ticks = now_ticks;
                    tfd->expirations++;
                    wait_queue_wake_all(&tfd->wq);
                } else {
                    tfd->next_expiry_ticks = abs_ticks;
                }
            } else {
                /* Relative time: add to current time */
                uint64_t rel_ticks = timespec_to_ticks(
                    new_value->it_value.tv_sec,
                    new_value->it_value.tv_nsec);

                if (rel_ticks == 0) {
                    /* Fire immediately */
                    tfd->next_expiry_ticks = now_ticks;
                    tfd->expirations++;
                    wait_queue_wake_all(&tfd->wq);
                } else {
                    tfd->next_expiry_ticks = now_ticks + rel_ticks;
                }
            }
        }
    }

    return 0;
}

int timerfd_gettime(int fd, struct itimerspec *curr_value)
{
    struct timerfd_info *tfd = timerfd_get(fd);
    if (!tfd || !curr_value) return -1;

    uint64_t now_ticks = timerfd_current_ticks(tfd->clockid);

    /* Return the interval */
    curr_value->it_interval = tfd->settings.it_interval;

    /* Return remaining time */
    if (tfd->next_expiry_ticks == 0 || tfd->next_expiry_ticks <= now_ticks) {
        curr_value->it_value.tv_sec = 0;
        curr_value->it_value.tv_nsec = 0;
    } else {
        uint64_t remaining_ticks = tfd->next_expiry_ticks - now_ticks;
        uint64_t remaining_ns = remaining_ticks * NS_PER_TICK;
        curr_value->it_value.tv_sec = remaining_ns / 1000000000ULL;
        curr_value->it_value.tv_nsec = remaining_ns % 1000000000ULL;
    }

    return 0;
}

int timerfd_read(int fd, uint64_t *val)
{
    struct timerfd_info *tfd = timerfd_get(fd);
    if (!tfd) return -1;

    for (;;) {
        uint64_t now_ticks = timerfd_current_ticks(tfd->clockid);

        /* Check for expiry */
        if (tfd->next_expiry_ticks > 0 && tfd->next_expiry_ticks <= now_ticks) {
            /* Timer expired */
            tfd->expirations++;
            /* Re-arm if interval timer */
            if (tfd->interval_ticks > 0) {
                /* Advance by one or more intervals to catch up */
                uint64_t elapsed = now_ticks - tfd->next_expiry_ticks;
                uint64_t periods = elapsed / tfd->interval_ticks;
                tfd->next_expiry_ticks += (periods + 1) * tfd->interval_ticks;
                /* If we still missed, keep advancing */
                while (tfd->next_expiry_ticks <= now_ticks)
                    tfd->next_expiry_ticks += tfd->interval_ticks;
            } else {
                /* One-shot: disarm */
                tfd->next_expiry_ticks = 0;
            }
        }

        if (tfd->expirations > 0) {
            *val = tfd->expirations;
            tfd->expirations = 0;
            return 0;
        }

        /* No expirations — non-blocking mode */
        if (tfd->flags & TFD_NONBLOCK) {
            return -1; /* EAGAIN */
        }

        /* Block until expiry */
        wait_queue_sleep(&tfd->wq);
    }
}

void timerfd_close(int fd)
{
    struct timerfd_info *tfd = timerfd_get(fd);
    if (!tfd) return;
    tfd->in_use = 0;
    tfd->next_expiry_ticks = 0;
    tfd->expirations = 0;
}

/* ── Stub: timerfd_poll ─────────────────────────────── */
int timerfd_poll(void *file, void *pt)
{
    (void)file;
    (void)pt;
    kprintf("[timerfd] timerfd_poll: not yet implemented\n");
    return 0;
}
/* ── Stub: timerfd_show_fdinfo ─────────────────────────────── */
static int timerfd_show_fdinfo(void *file, void *m)
{
    (void)file;
    (void)m;
    kprintf("[timerfd] timerfd_show_fdinfo: not yet implemented\n");
    return 0;
}
