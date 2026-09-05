/*
 * posix_timer.c — POSIX timer and clock syscall implementations
 *
 * Implements the clock_gettime / clock_settime / clock_getres family
 * and the per-process POSIX timer (timer_create / timer_settime /
 * timer_gettime / timer_getoverrun / timer_delete) syscalls.
 *
 * Clock sources:
 *   CLOCK_REALTIME          — wall-clock time via RTC epoch + uptime
 *   CLOCK_MONOTONIC         — time since boot (includes time-ns offsets)
 *   CLOCK_PROCESS_CPUTIME_ID — per-process CPU time (user + system)
 *   CLOCK_THREAD_CPUTIME_ID  — per-thread CPU time (same as process)
 *   CLOCK_BOOTTIME          — monotonic including suspend
 *
 * These functions are called from the syscall dispatch in syscall.c.
 */

#define KERNEL_INTERNAL
#include "caps.h"
#include "module.h"
#include "printf.h"
#include "process.h"
#include "rtc.h"
#include "scheduler.h"
#include "spinlock.h"
#include "string.h"
#include "syscall.h"
#include "timekeeping.h"
#include "timer.h"
#include "timers.h"
#include "types.h"
#include "uaccess.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION(
    "POSIX timer and clock syscall implementations — clock_gettime, timer_create, etc.");
MODULE_AUTHOR("Ruslan Gustomiasov");

/* ── Clock identifiers (standard POSIX / Linux values) ─────────── */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE 5
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 6
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif
#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif

/* ── POSIX timer slots ─────────────────────────────────────────── */
#define POSIX_TIMER_MAX 16
#define MAX_TIMERS POSIX_TIMER_MAX

struct posix_timer {
    int in_use;
    int clockid;
    int signo;            /* signal to deliver on expiry */
    int notify;           /* SIGEV_SIGNAL, SIGEV_NONE */
    uint64_t it_value;    /* ticks to first expiry */
    uint64_t it_interval; /* ticks between repeats */
    uint64_t start_tick;  /* creation/arm tick */
    uint64_t overrun;     /* overrun count */
    uint32_t pid;         /* target process */
};

static struct posix_timer posix_timers[POSIX_TIMER_MAX];

/* ── Helper: convert ticks to struct timespec ──────────────────── */
static void ticks_to_timespec(uint64_t ticks, struct timespec *ts) {
    ts->tv_sec = ticks / TIMER_FREQ;
    ts->tv_nsec = (ticks % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
}

/* ── Helper: apply time-namespace monotonic offset ─────────────── */
static void apply_mono_offset(struct timespec *ts, struct process *cur) {
    if (!cur || !(cur->ns_flags & CLONE_NEWTIME))
        return;

    int64_t off = cur->timens_mono_offset;
    if (off == 0)
        return;

    int64_t new_ns = (int64_t)ts->tv_nsec + off;
    while (new_ns >= 1000000000LL) {
        ts->tv_sec++;
        new_ns -= 1000000000LL;
    }
    while (new_ns < 0) {
        if (ts->tv_sec > 0) {
            ts->tv_sec--;
            new_ns += 1000000000LL;
        } else {
            new_ns = 0;
            break;
        }
    }
    ts->tv_nsec = (uint64_t)new_ns;

    int64_t off_sec = off / 1000000000LL;
    if (off_sec != 0) {
        if (off_sec > 0 || (int64_t)ts->tv_sec >= -off_sec)
            ts->tv_sec = (uint64_t)((int64_t)ts->tv_sec + off_sec);
        else
            ts->tv_sec = 0;
    }
}

/* ── Helper: apply time-namespace boottime offset ──────────────── */
static void apply_boottime_offset(struct timespec *ts, struct process *cur) {
    if (!cur || !(cur->ns_flags & CLONE_NEWTIME))
        return;

    int64_t off = cur->timens_boottime_offset;
    if (off == 0)
        return;

    int64_t new_ns = (int64_t)ts->tv_nsec + off;
    while (new_ns >= 1000000000LL) {
        ts->tv_sec++;
        new_ns -= 1000000000LL;
    }
    while (new_ns < 0) {
        if (ts->tv_sec > 0) {
            ts->tv_sec--;
            new_ns += 1000000000LL;
        } else {
            new_ns = 0;
            break;
        }
    }
    ts->tv_nsec = (uint64_t)new_ns;

    int64_t off_sec = off / 1000000000LL;
    if (off_sec != 0) {
        if (off_sec > 0 || (int64_t)ts->tv_sec >= -off_sec)
            ts->tv_sec = (uint64_t)((int64_t)ts->tv_sec + off_sec);
        else
            ts->tv_sec = 0;
    }
}

/* ── sys_clock_gettime ───────────────────────────────────────────
 *
 *   clock_gettime(clockid, struct timespec *tp)
 *
 * Supported clocks:
 *   CLOCK_REALTIME            — wall clock (RTC epoch + uptime)
 *   CLOCK_MONOTONIC           — time since boot (with time-ns offset)
 *   CLOCK_MONOTONIC_RAW       — raw monotonic (no offset)
 *   CLOCK_MONOTONIC_COARSE    — same as MONOTONIC (coarse grain)
 *   CLOCK_BOOTTIME            — monotonic including suspend
 *   CLOCK_PROCESS_CPUTIME_ID  — CPU time consumed by this process
 *   CLOCK_THREAD_CPUTIME_ID   — CPU time consumed by this thread
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINVAL on invalid clockid.
 */
int64_t sys_clock_gettime(uint64_t clockid, uint64_t tp_addr) {
    struct timespec ts;
    uint64_t ticks = timer_get_ticks();
    struct process *cur = process_get_current();

    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_TAI: {
        uint64_t epoch = rtc_get_epoch();
        int64_t ns;
        int64_t off = timekeeping_get_rt_offset();
        ts.tv_sec = epoch + (ticks / TIMER_FREQ);
        ts.tv_nsec = (ticks % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
        /* Fold any NTP-applied slew offset into the reported time,
         * carrying into the seconds field to keep tv_nsec in range. */
        ns = (int64_t)ts.tv_nsec + off;
        while (ns >= 1000000000LL) {
            ts.tv_sec += 1;
            ns -= 1000000000LL;
        }
        while (ns < 0) {
            ts.tv_sec -= 1;
            ns += 1000000000LL;
        }
        ts.tv_nsec = (uint64_t)ns;
        /* CLOCK_TAI = CLOCK_REALTIME + the TAI-UTC leap-second offset. */
        if (clockid == CLOCK_TAI)
            ts.tv_sec += (int64_t)timekeeping_leap_offset((uint64_t)ts.tv_sec);
        break;
    }

    case CLOCK_REALTIME_COARSE:
        /* Coarse clock: serve the per-tick cached wall-clock snapshot. */
        timekeeping_coarse_realtime(&ts);
        break;

    case CLOCK_MONOTONIC:
        ticks_to_timespec(ticks, &ts);
        apply_mono_offset(&ts, cur);
        break;

    case CLOCK_MONOTONIC_RAW:
        ticks_to_timespec(ticks, &ts);
        break;

    case CLOCK_MONOTONIC_COARSE:
        /* Coarse clock: serve the per-tick cached monotonic snapshot. */
        timekeeping_coarse_monotonic(&ts);
        break;

    case CLOCK_BOOTTIME:
    case CLOCK_BOOTTIME_ALARM:
        ticks_to_timespec(ticks, &ts);
        apply_boottime_offset(&ts, cur);
        break;

    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
        if (!cur)
            return (uint64_t)(int64_t)-EINVAL;
        uint64_t total_ticks = cur->utime_ticks + cur->stime_ticks;
        ticks_to_timespec(total_ticks, &ts);
        break;
    }

    default:
        /* Unsupported clock ID — return -EINVAL (Linux convention) */
        return (uint64_t)(int64_t)-EINVAL;
    }

    if (copy_to_user(tp_addr, &ts, sizeof(struct timespec)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    return 0;
}

/* ── sys_clock_settime ───────────────────────────────────────────
 *
 *   clock_settime(clockid, const struct timespec *tp)
 *
 * Only CLOCK_REALTIME (and CLOCK_REALTIME_COARSE) are settable,
 * and only when the caller has CAP_SYS_TIME.  Adjusts the boot
 * epoch so that rtc_get_epoch() + ticks_since_boot reflects the
 * new wall-clock time.
 *
 * Returns: 0 on success, -EPERM if not privileged, -EFAULT on
 * bad pointer, -EINVAL on invalid clockid or invalid tv_nsec.
 */
int64_t sys_clock_settime(uint64_t clockid, uint64_t tp_addr) {
    /* Only realtime clocks are settable */
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_REALTIME_COARSE &&
        clockid != CLOCK_REALTIME_ALARM)
        return (uint64_t)(int64_t)-EINVAL;

    /* CLOCK_REALTIME_ALARM requires CAP_WAKE_ALARM;
     * all other settable clocks require CAP_SYS_TIME. */
    if (clockid == CLOCK_REALTIME_ALARM) {
        if (cap_capable_audit(CAP_WAKE_ALARM, "clock_settime") < 0)
            return (uint64_t)(int64_t)-EPERM;
    } else {
        if (cap_capable_audit(CAP_SYS_TIME, "clock_settime") < 0)
            return (uint64_t)(int64_t)-EPERM;
    }

    struct timespec ts;
    if (copy_from_user(&ts, tp_addr, sizeof(struct timespec)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Validate timespec: tv_nsec must be in [0, 999999999] */
    if (ts.tv_nsec >= 1000000000ULL)
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t ticks = timer_get_ticks();
    uint64_t ticks_sec = ticks / TIMER_FREQ;

    /*
     * Compute new boot epoch so that:
     *   current_wall_time = boot_epoch + ticks_since_boot
     *   => new_boot_epoch = desired_time - ticks_since_boot
     *
     * If the desired time is before the boot (can't happen in
     * practice with a valid epoch), clamp to zero.
     */
    uint64_t new_epoch = (ts.tv_sec >= ticks_sec) ? (ts.tv_sec - ticks_sec) : 0;
    rtc_set_epoch(new_epoch);

    /* Persist the new wall-clock time to the CMOS RTC so it survives
     * reboot (the RTC is the persistent clock backing boot_epoch). */
    rtc_update_clock();

    return 0;
}

/* ── sys_settimeofday ───────────────────────────────────────────
 *
 *   settimeofday(struct timeval *tv, struct timezone *tz)
 *
 * Sets the realtime clock from a timeval (second + microsecond), or
 * merely queries it when tv is NULL.  Requires CAP_SYS_TIME.
 * Returns 0 on success, -EFAULT / -EINVAL / -EPERM on error.
 */
int64_t sys_settimeofday(uint64_t tv_addr, uint64_t tz_addr) {
    uint64_t ticks = timer_get_ticks();
    uint64_t ticks_sec = ticks / TIMER_FREQ;

    /* If tz is supplied, accept (and ignore) it — the kernel has no
     * per-process timezone state; it is informational only. */
    if (tz_addr) {
        struct {
            int32_t tz_minuteswest;
            int32_t tz_dsttime;
        } tz;
        if (copy_from_user(&tz, tz_addr, sizeof(tz)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    /* tv == NULL is a query: return 0 without changing the clock. */
    if (!tv_addr)
        return 0;

    struct timeval tv;
    if (copy_from_user(&tv, tv_addr, sizeof(struct timeval)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    if (tv.tv_usec >= 1000000ULL)
        return (uint64_t)(int64_t)-EINVAL;

    if (cap_capable_audit(CAP_SYS_TIME, "settimeofday") < 0)
        return (uint64_t)(int64_t)-EPERM;

    /* Rebase boot epoch from the desired wall-clock seconds. */
    uint64_t new_epoch = (tv.tv_sec >= ticks_sec) ? (tv.tv_sec - ticks_sec) : 0;
    rtc_set_epoch(new_epoch);

    /* Fold the microsecond remainder in as a one-off slew so the
     * reported time includes the fractional part. */
    timekeeping_set_rt_offset((int64_t)tv.tv_usec * 1000);

    rtc_update_clock();
    return 0;
}

/* ── sys_adjtimex ───────────────────────────────────────────────
 *
 *   adjtimex(struct timex *tx)
 *
 * Reads and/or adjusts the kernel clock via the Linux struct timex.
 * With modes == 0 this is a pure query that returns current parameters.
 * Supported adjustment modes:
 *   ADJ_OFFSET   — slew the clock by the given microsecond offset
 *   ADJ_STATUS   — set clock status bits
 *   ADJ_MAXERROR — set maximum error
 *   ADJ_ESTERROR — set estimated error
 *   ADJ_TAI      — does not adjust (TAI derived from leap table)
 * Returns 0 on success, -EFAULT on bad pointer.
 */
int64_t sys_adjtimex(uint64_t tx_addr) {
    struct timex tx;
    if (!tx_addr)
        return (uint64_t)(int64_t)-EFAULT;
    if (copy_from_user(&tx, tx_addr, sizeof(struct timex)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    uint64_t ticks = timer_get_ticks();
    uint64_t epoch = rtc_get_epoch();
    int64_t now_sec = (int64_t)(epoch + (ticks / TIMER_FREQ));

    /* Default fields filled on every call (query or adjust). */
    tx.maxerror = 16000000;  /* 16 s maximum error */
    tx.esterror = 500000;    /* 0.5 s estimated error */
    tx.precision = 10000;    /* 10 ms tick precision (us) */
    tx.tolerance = 32896000; /* 32.7 ppm scaled by 65536 */
    tx.tick = 10000;         /* 10 ms = 10000 us per tick */
    tx.constant = 0;
    tx.ppsfreq = 0;
    tx.jitter = 0;
    tx.shift = 0;
    tx.stabil = 0;
    tx.jitcnt = 0;
    tx.calcnt = 0;
    tx.errcnt = 0;
    tx.stbcnt = 0;
    tx.time.tv_sec = (uint64_t)now_sec;
    tx.time.tv_usec = (uint64_t)((ticks % TIMER_FREQ) * (1000000ULL / TIMER_FREQ));
    tx.tai = timekeeping_leap_offset((uint64_t)now_sec);

    /* Apply requested adjustments. */
    if (tx.modes & ADJ_OFFSET) {
        if (cap_capable_audit(CAP_SYS_TIME, "adjtimex") < 0)
            return (uint64_t)(int64_t)-EPERM;
        timekeeping_set_rt_offset(tx.offset * 1000); /* us → ns */
    }
    if (tx.modes & ADJ_STATUS) {
        tx.status = tx.status; /* status accepted and reported back */
    }
    if (tx.modes & ADJ_TAI) {
        /* TAI is derived from the leap-second table; we accept the field
         * but do not let userspace override the physical TAI offset. */
    }

    if (copy_to_user(tx_addr, &tx, sizeof(struct timex)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

/* ── sys_clock_getres ────────────────────────────────────────────
 *
 *   clock_getres(clockid, struct timespec *res)
 *
 * Returns the resolution of the given clock.  The kernel timer runs
 * at TIMER_FREQ Hz (100 Hz → 10 ms resolution), so all supported
 * clocks return the same resolution.
 *
 * Supported clock IDs: CLOCK_REALTIME, CLOCK_REALTIME_COARSE,
 * CLOCK_REALTIME_ALARM, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
 * CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME, CLOCK_BOOTTIME_ALARM,
 * CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID.
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINVAL on
 * invalid clockid.
 */
int64_t sys_clock_getres(uint64_t clockid, uint64_t res_addr) {
    /* Validate the clock ID */
    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_BOOTTIME_ALARM:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        break;
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }

    if (res_addr) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = NS_PER_TICK; /* 10 ms — tick-level resolution */
        if (copy_to_user(res_addr, &ts, sizeof(struct timespec)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    return 0;
}

/* ── TIMER_ABSTIME flag for clock_nanosleep ──────────────────── */
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* ── sys_clock_nanosleep ──────────────────────────────────────────
 *
 *   clock_nanosleep(clockid, flags, const struct timespec *req,
 *                   struct timespec *rem)
 *
 * High-resolution sleep with support for both relative and absolute
 * deadlines.  Supported clocks: CLOCK_REALTIME, CLOCK_MONOTONIC,
 * CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME.
 *
 * If flags & TIMER_ABSTIME, req is an absolute time according to
 * the given clock; otherwise it is a relative interval.
 *
 * If the sleep is interrupted by a signal and rem is non-NULL, the
 * remaining time is written back.
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINTR if
 * interrupted by a signal, -EINVAL on invalid clockid or
 * invalid tv_nsec.
 */
int64_t sys_clock_nanosleep(uint64_t clockid, uint64_t flags, uint64_t req_addr,
                            uint64_t rem_addr) {
    struct process *proc;
    struct timespec req;
    uint64_t now;
    uint64_t deadline; /* target boot tick */
    uint64_t ticks;

    /* Validate clockid */
    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_BOOTTIME_ALARM:
        break;
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }

    /* Copy request from user space */
    if (copy_from_user(&req, req_addr, sizeof(struct timespec)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Validate tv_nsec */
    if (req.tv_nsec >= 1000000000ULL)
        return (uint64_t)(int64_t)-EINVAL;

    proc = process_get_current();
    if (!proc)
        return (uint64_t)(int64_t)-EINTR;

    now = timer_get_ticks();

    if (flags & TIMER_ABSTIME) {
        /* Absolute deadline */
        switch (clockid) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE: {
            /*
             * Convert wall-clock absolute time to boot ticks.
             *   deadline = (req_sec - boot_epoch) * TIMER_FREQ
             *            + req_nsec / NS_PER_TICK
             */
            uint64_t epoch = rtc_get_epoch();
            uint64_t req_sec = req.tv_sec;
            if (req_sec <= epoch) {
                deadline = 0; /* already passed */
            } else {
                deadline = (req_sec - epoch) * TIMER_FREQ + req.tv_nsec / NS_PER_TICK;
            }
            break;
        }

        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
        case CLOCK_BOOTTIME_ALARM:
            /* Monotonic absolute time is already in boot ticks */
            deadline = req.tv_sec * TIMER_FREQ + req.tv_nsec / NS_PER_TICK;
            break;

        default:
            return (uint64_t)(int64_t)-EINVAL;
        }

        /* If deadline already passed, return 0 immediately */
        if (deadline <= now)
            return 0;

    } else {
        /* Relative interval */
        ticks = req.tv_sec * TIMER_FREQ + req.tv_nsec / NS_PER_TICK;
        if (ticks == 0 && req.tv_nsec > 0)
            ticks = 1; /* minimum 1 tick */

        deadline = now + ticks;
    }

    /* Block the process until deadline, restarting if SA_RESTART is set */
    for (;;) {
        proc->sleep_until = deadline;
        proc->state = PROCESS_BLOCKED;
        scheduler_remove(proc);

        /*
         * Lost-wakeup guard: the timer-IRQ wake scan (scheduler_wake_sleepers)
         * can fire between the state write above and scheduler_remove().  It
         * clears sleep_until, marks the process PROCESS_READY and queues it —
         * which scheduler_remove() then undoes, leaving the process READY but
         * off the runqueue with sleep_until == 0.  No wake source ever touches
         * it again (the wake scan needs BLOCKED + sleep_until > 0) and it
         * sleeps forever.  Snapshot under sched_lock — held by the wake scan
         * across its whole check-and-queue — to detect an already-delivered
         * wake: if it fired before our remove un-queued us, keep running
         * instead of yielding.
         */
        uint64_t __sleep_flags;
        spinlock_irqsave_acquire(&sched_lock, &__sleep_flags);
        int wake_delivered = (proc->sleep_until == 0 && proc->state == PROCESS_READY);
        int wake_queued = proc->on_queue;
        spinlock_irqsave_release(&sched_lock, __sleep_flags);

        if (wake_delivered && !wake_queued)
            proc->state = PROCESS_RUNNING; /* wake already delivered — keep running */
        else
            scheduler_yield();

        /* Process woke up — check if timer expired or signal */
        now = timer_get_ticks();
        if (now >= deadline)
            return 0;

        /* Woken early by signal — check SA_RESTART */
        if (proc->pending_signals && signal_has_sa_restart())
            continue; /* restart the sleep (SA_RESTART) */

        /* Compute remaining time and return -EINTR */
        uint64_t remaining = deadline - now;
        if (rem_addr) {
            struct timespec rem;
            rem.tv_sec = remaining / TIMER_FREQ;
            rem.tv_nsec = (remaining % TIMER_FREQ) * NS_PER_TICK;
            if (copy_to_user(rem_addr, &rem, sizeof(struct timespec)) < 0)
                return (uint64_t)(int64_t)-EFAULT;
        }
        return (uint64_t)(int64_t)-EINTR;
    }

    return 0;
}

/* ── sys_timer_create ────────────────────────────────────────────
 *
 *   timer_create(clockid, struct sigevent *sevp, timer_t *timerid)
 *
 * Allocates a per-process POSIX timer slot.  The timer is initially
 * disarmed (it_value == 0).  Returns the timer ID via timerid.
 *
 * Supported clock IDs: CLOCK_REALTIME, CLOCK_REALTIME_COARSE,
 * CLOCK_REALTIME_ALARM, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
 * CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME, CLOCK_BOOTTIME_ALARM,
 * CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID.
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINVAL on invalid
 * clockid or invalid sigevent, -EAGAIN if no timer slots available.
 */
int64_t sys_timer_create(uint64_t clockid, uint64_t sevp_addr, uint64_t timerid_addr) {
    struct sigevent sev;
    int sig = SIGALRM; /* default signal */
    int notify = SIGEV_SIGNAL;

    /* Validate clockid */
    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_REALTIME_ALARM:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
    case CLOCK_BOOTTIME_ALARM:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        break;
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }

    /* ALARM clocks (CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM) can
     * wake the system from suspend, so arming a timer on them requires
     * CAP_WAKE_ALARM — the same gate clock_settime() enforces. */
    if (clockid == CLOCK_REALTIME_ALARM || clockid == CLOCK_BOOTTIME_ALARM) {
        if (cap_capable_audit(CAP_WAKE_ALARM, "timer_create") < 0)
            return (uint64_t)(int64_t)-EPERM;
    }

    if (sevp_addr) {
        if (copy_from_user(&sev, sevp_addr, sizeof(struct sigevent)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        notify = sev.sigev_notify;

        /* Validate notification type */
        switch (notify) {
        case SIGEV_SIGNAL:
            /* Signal number must be in valid range (1..31, 34..64) */
            if (sev.sigev_signo < 1 || (sev.sigev_signo > 31 && sev.sigev_signo < 34) ||
                sev.sigev_signo > 64)
                return (uint64_t)(int64_t)-EINVAL;
            sig = sev.sigev_signo;
            break;
        case SIGEV_NONE:
            sig = 0;
            break;
        case SIGEV_THREAD:
            /*
             * SIGEV_THREAD requires thread creation — not yet
             * implemented in this kernel.  Fall through to -EINVAL.
             */
        default:
            return (uint64_t)(int64_t)-EINVAL;
        }
    }

    struct process *cur = process_get_current();

    for (int i = 0; i < POSIX_TIMER_MAX; i++) {
        if (!posix_timers[i].in_use) {
            posix_timers[i].in_use = 1;
            posix_timers[i].clockid = (int)clockid;
            posix_timers[i].signo = sig;
            posix_timers[i].notify = notify;
            posix_timers[i].it_value = 0;
            posix_timers[i].it_interval = 0;
            posix_timers[i].start_tick = 0;
            posix_timers[i].overrun = 0;
            posix_timers[i].pid = cur ? cur->pid : 0;

            timer_t tid = (timer_t)(i + 1);
            if (copy_to_user(timerid_addr, &tid, sizeof(timer_t)) < 0)
                return (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
    }

    return (uint64_t)(int64_t)-EAGAIN; /* no free timer slot */
}

/* ── sys_timer_settime ───────────────────────────────────────────
 *
 *   timer_settime(timerid, flags, const struct itimerspec *new,
 *                 struct itimerspec *old)
 *
 * Arms or disarms the timer.  If new->it_value is zero the timer is
 * disarmed.  If flags & TIMER_ABSTIME the value is interpreted as
 * absolute time; otherwise it is a relative interval.
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINVAL on invalid
 * timerid or invalid timespec fields.
 */
int64_t sys_timer_settime(uint64_t timerid, uint64_t flags, uint64_t new_addr, uint64_t old_addr) {
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    struct itimerspec new_val;
    memset(&new_val, 0, sizeof(new_val));
    if (new_addr) {
        if (copy_from_user(&new_val, new_addr, sizeof(struct itimerspec)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        /* Validate timespec fields */
        if (new_val.it_value.tv_nsec >= 1000000000ULL ||
            new_val.it_interval.tv_nsec >= 1000000000ULL)
            return (uint64_t)(int64_t)-EINVAL;
    }

    /* Return old timer value before overwriting */
    if (old_addr) {
        struct itimerspec old;
        old.it_interval.tv_sec = posix_timers[idx].it_interval / TIMER_FREQ;
        old.it_interval.tv_nsec = (posix_timers[idx].it_interval % TIMER_FREQ) * NS_PER_TICK;
        old.it_value.tv_sec = posix_timers[idx].it_value / TIMER_FREQ;
        old.it_value.tv_nsec = (posix_timers[idx].it_value % TIMER_FREQ) * NS_PER_TICK;
        if (copy_to_user(old_addr, &old, sizeof(struct itimerspec)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    if (new_addr) {
        uint64_t now = timer_get_ticks();

        if ((flags & TIMER_ABSTIME) && new_val.it_value.tv_sec > 0) {
            /*
             * Absolute deadline — convert to relative ticks.
             * The timer's clockid determines the time domain.
             */
            switch (posix_timers[idx].clockid) {
            case CLOCK_REALTIME:
            case CLOCK_REALTIME_COARSE:
            case CLOCK_REALTIME_ALARM: {
                uint64_t epoch = rtc_get_epoch();
                if (new_val.it_value.tv_sec > epoch) {
                    uint64_t deadline = (new_val.it_value.tv_sec - epoch) * TIMER_FREQ +
                                        new_val.it_value.tv_nsec / NS_PER_TICK;
                    posix_timers[idx].it_value = (deadline > now) ? (deadline - now) : 0;
                } else {
                    posix_timers[idx].it_value = 0; /* already passed */
                }
                break;
            }

            case CLOCK_MONOTONIC:
            case CLOCK_MONOTONIC_RAW:
            case CLOCK_MONOTONIC_COARSE:
            case CLOCK_BOOTTIME:
            case CLOCK_BOOTTIME_ALARM:
            case CLOCK_PROCESS_CPUTIME_ID:
            case CLOCK_THREAD_CPUTIME_ID: {
                uint64_t deadline =
                    new_val.it_value.tv_sec * TIMER_FREQ + new_val.it_value.tv_nsec / NS_PER_TICK;
                posix_timers[idx].it_value = (deadline > now) ? (deadline - now) : 0;
                break;
            }

            default:
                return (uint64_t)(int64_t)-EINVAL;
            }
        } else {
            /* Relative interval */
            posix_timers[idx].it_value =
                new_val.it_value.tv_sec * TIMER_FREQ + new_val.it_value.tv_nsec / NS_PER_TICK;
        }

        posix_timers[idx].it_interval =
            new_val.it_interval.tv_sec * TIMER_FREQ + new_val.it_interval.tv_nsec / NS_PER_TICK;
        posix_timers[idx].start_tick = now;
        posix_timers[idx].overrun = 0;
    }

    return 0;
}

/* ── sys_timer_gettime ───────────────────────────────────────────
 *
 *   timer_gettime(timerid, struct itimerspec *cur)
 *
 * Returns the remaining time until expiry and the reload interval.
 *
 * Returns: 0 on success, -EFAULT on bad pointer, -EINVAL on invalid
 * timerid.
 */
int64_t sys_timer_gettime(uint64_t timerid, uint64_t cur_addr) {
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    struct itimerspec cur;
    uint64_t now = timer_get_ticks();
    uint64_t elapsed =
        (now >= posix_timers[idx].start_tick) ? (now - posix_timers[idx].start_tick) : 0;
    uint64_t remaining =
        posix_timers[idx].it_value > elapsed ? posix_timers[idx].it_value - elapsed : 0;

    cur.it_interval.tv_sec = posix_timers[idx].it_interval / TIMER_FREQ;
    cur.it_interval.tv_nsec = (posix_timers[idx].it_interval % TIMER_FREQ) * NS_PER_TICK;
    cur.it_value.tv_sec = remaining / TIMER_FREQ;
    cur.it_value.tv_nsec = (remaining % TIMER_FREQ) * NS_PER_TICK;

    if (copy_to_user(cur_addr, &cur, sizeof(struct itimerspec)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    return 0;
}

/* ── sys_timer_getoverrun ────────────────────────────────────────
 *
 *   timer_getoverrun(timerid)
 *
 * Returns the overrun count (number of extra expirations that
 * occurred between the signal delivery and the timer_getoverrun
 * call).  The overrun is reset to 0 after reading, per POSIX.
 *
 * Returns: overrun count (capped at DELAYTIMER_MAX, 0x7FFFFFFF)
 * on success, -EINVAL on invalid timerid.
 */
int64_t sys_timer_getoverrun(uint64_t timerid) {
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t overrun = posix_timers[idx].overrun;

    /* Reset overrun after reading (POSIX semantics) */
    posix_timers[idx].overrun = 0;

    /* Cap at DELAYTIMER_MAX (2147483647) as required by POSIX */
    if (overrun > 2147483647ULL)
        overrun = 2147483647ULL;

    return overrun;
}

/* ── sys_timer_delete ────────────────────────────────────────────
 *
 *   timer_delete(timerid)
 *
 * Destroys a POSIX per-process timer, freeing its slot.  All
 * fields are zeroed to prevent stale data access.
 *
 * Returns: 0 on success, -EINVAL on invalid timerid.
 */
int64_t sys_timer_delete(uint64_t timerid) {
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    /* Clear all fields to prevent stale data */
    memset(&posix_timers[idx], 0, sizeof(struct posix_timer));
    return 0;
}

/* ── posix_timer_init ───────────────────────────────────────────
 *
 * Called during boot from production_subsystems_init() to clear
 * the POSIX timer table.
 */
void posix_timer_init(void) {
    memset(posix_timers, 0, sizeof(posix_timers));
}

/* ── posix_timer_tick ────────────────────────────────────────────
 *
 * Called from the timer interrupt (timer.c) on every tick.
 * Checks all armed POSIX timers and delivers signals on expiry.
 */
void posix_timer_tick(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < POSIX_TIMER_MAX; i++) {
        if (!posix_timers[i].in_use || posix_timers[i].it_value == 0)
            continue;

        uint64_t elapsed = now - posix_timers[i].start_tick;
        if (elapsed >= posix_timers[i].it_value) {
            /* Send signal to the timer's process */
            if (posix_timers[i].signo > 0 && posix_timers[i].pid) {
                signal_send(posix_timers[i].pid, posix_timers[i].signo);
            }

            if (posix_timers[i].it_interval > 0) {
                /* Periodic timer: compute overruns and re-arm */
                uint64_t overruns = elapsed / posix_timers[i].it_value;
                posix_timers[i].overrun += overruns - 1;
                posix_timers[i].start_tick = now;
                posix_timers[i].it_value = posix_timers[i].it_interval;
            } else {
                /* One-shot: disarm */
                posix_timers[i].it_value = 0;
            }
        }
    }
}
