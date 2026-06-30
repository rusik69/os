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
#include "syscall.h"
#include "types.h"
#include "timer.h"
#include "timers.h"
#include "rtc.h"
#include "process.h"
#include "uaccess.h"
#include "caps.h"
#include "scheduler.h"
#include "string.h"
#include "printf.h"
#include "module.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("POSIX timer and clock syscall implementations — clock_gettime, timer_create, etc.");
MODULE_AUTHOR("Ruslan Gustomiasov");

/* ── Clock identifiers (standard POSIX / Linux values) ─────────── */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME                  0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC                 1
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID        2
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID         3
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW             4
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE           5
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE          6
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME                  7
#endif
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM            8
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM            9
#endif

/* ── POSIX timer slots ─────────────────────────────────────────── */
#define POSIX_TIMER_MAX 16
#define MAX_TIMERS POSIX_TIMER_MAX

struct posix_timer {
    int      in_use;
    int      clockid;
    int      signo;           /* signal to deliver on expiry */
    uint64_t it_value;        /* ticks to first expiry */
    uint64_t it_interval;     /* ticks between repeats */
    uint64_t start_tick;      /* creation/arm tick */
    uint64_t overrun;         /* overrun count */
    uint32_t pid;             /* target process */
};

static struct posix_timer posix_timers[POSIX_TIMER_MAX];

/* ── Helper: convert ticks to struct timespec ──────────────────── */
static void ticks_to_timespec(uint64_t ticks, struct timespec *ts)
{
    ts->tv_sec  = ticks / TIMER_FREQ;
    ts->tv_nsec = (ticks % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
}

/* ── Helper: apply time-namespace monotonic offset ─────────────── */
static void apply_mono_offset(struct timespec *ts, struct process *cur)
{
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
static void apply_boottime_offset(struct timespec *ts, struct process *cur)
{
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
uint64_t sys_clock_gettime(uint64_t clockid, uint64_t tp_addr)
{
    struct timespec ts;
    uint64_t ticks = timer_get_ticks();
    struct process *cur = process_get_current();

    switch (clockid) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
    case CLOCK_REALTIME_ALARM: {
        uint64_t epoch = rtc_get_epoch();
        ts.tv_sec  = epoch + (ticks / TIMER_FREQ);
        ts.tv_nsec = (ticks % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
        break;
    }

    case CLOCK_MONOTONIC:
        ticks_to_timespec(ticks, &ts);
        apply_mono_offset(&ts, cur);
        break;

    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
        ticks_to_timespec(ticks, &ts);
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
uint64_t sys_clock_settime(uint64_t clockid, uint64_t tp_addr)
{
    /* Only realtime clocks are settable */
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_REALTIME_COARSE
        && clockid != CLOCK_REALTIME_ALARM)
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
    uint64_t new_epoch = (ts.tv_sec >= ticks_sec)
                         ? (ts.tv_sec - ticks_sec) : 0;
    rtc_set_epoch(new_epoch);
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
uint64_t sys_clock_getres(uint64_t clockid, uint64_t res_addr)
{
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
        ts.tv_sec  = 0;
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
uint64_t sys_clock_nanosleep(uint64_t clockid, uint64_t flags,
                             uint64_t req_addr, uint64_t rem_addr)
{
    struct process *proc;
    struct timespec req;
    uint64_t now;
    uint64_t deadline;   /* target boot tick */
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
                deadline = 0;  /* already passed */
            } else {
                deadline = (req_sec - epoch) * TIMER_FREQ
                         + req.tv_nsec / NS_PER_TICK;
            }
            break;
        }

        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
        case CLOCK_BOOTTIME_ALARM:
            /* Monotonic absolute time is already in boot ticks */
            deadline = req.tv_sec * TIMER_FREQ
                     + req.tv_nsec / NS_PER_TICK;
            break;

        default:
            return (uint64_t)(int64_t)-EINVAL;
        }

        /* If deadline already passed, return 0 immediately */
        if (deadline <= now)
            return 0;

    } else {
        /* Relative interval */
        ticks = req.tv_sec * TIMER_FREQ
              + req.tv_nsec / NS_PER_TICK;
        if (ticks == 0 && req.tv_nsec > 0)
            ticks = 1;  /* minimum 1 tick */

        deadline = now + ticks;
    }

    /* Block the process until deadline */
    proc->sleep_until = deadline;
    proc->state = PROCESS_BLOCKED;
    scheduler_remove(proc);
    scheduler_yield();

    /* Process woke up — check if timer expired or signal */
    now = timer_get_ticks();
    if (now < deadline) {
        /* Woken early by signal — compute remaining time */
        uint64_t remaining = deadline - now;
        if (rem_addr) {
            struct timespec rem;
            rem.tv_sec  = remaining / TIMER_FREQ;
            rem.tv_nsec = (remaining % TIMER_FREQ) * NS_PER_TICK;
            if (copy_to_user(rem_addr, &rem,
                             sizeof(struct timespec)) < 0)
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
 */
uint64_t sys_timer_create(uint64_t clockid, uint64_t sevp_addr,
                          uint64_t timerid_addr)
{
    struct sigevent sev;
    int sig = SIGALRM; /* default signal */

    if (sevp_addr) {
        if (copy_from_user(&sev, sevp_addr, sizeof(struct sigevent)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        sig = sev.sigev_signo;
    }

    struct process *cur = process_get_current();

    for (int i = 0; i < POSIX_TIMER_MAX; i++) {
        if (!posix_timers[i].in_use) {
            posix_timers[i].in_use = 1;
            posix_timers[i].clockid = (int)clockid;
            posix_timers[i].signo = sig;
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
 */
uint64_t sys_timer_settime(uint64_t timerid, uint64_t flags,
                           uint64_t new_addr, uint64_t old_addr)
{
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    struct itimerspec new_val;
    memset(&new_val, 0, sizeof(new_val));
    if (new_addr) {
        if (copy_from_user(&new_val, new_addr, sizeof(struct itimerspec)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    /* Return old timer value before overwriting */
    if (old_addr) {
        struct itimerspec old;
        old.it_interval.tv_sec  = posix_timers[idx].it_interval / TIMER_FREQ;
        old.it_interval.tv_nsec = (posix_timers[idx].it_interval % TIMER_FREQ)
                                  * (1000000000ULL / TIMER_FREQ);
        old.it_value.tv_sec     = posix_timers[idx].it_value / TIMER_FREQ;
        old.it_value.tv_nsec    = (posix_timers[idx].it_value % TIMER_FREQ)
                                  * (1000000000ULL / TIMER_FREQ);
        if (copy_to_user(old_addr, &old, sizeof(struct itimerspec)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    if (new_addr) {
        uint64_t val_ticks = new_val.it_value.tv_sec * TIMER_FREQ
                           + new_val.it_value.tv_nsec
                             / (1000000000ULL / TIMER_FREQ);
        uint64_t interval_ticks = new_val.it_interval.tv_sec * TIMER_FREQ
                                + new_val.it_interval.tv_nsec
                                  / (1000000000ULL / TIMER_FREQ);
        posix_timers[idx].it_value    = val_ticks;
        posix_timers[idx].it_interval = interval_ticks;
        posix_timers[idx].start_tick  = timer_get_ticks();
        posix_timers[idx].overrun     = 0;
    }

    return 0;
}

/* ── sys_timer_gettime ───────────────────────────────────────────
 *
 *   timer_gettime(timerid, struct itimerspec *cur)
 *
 * Returns the remaining time until expiry and the reload interval.
 */
uint64_t sys_timer_gettime(uint64_t timerid, uint64_t cur_addr)
{
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    struct itimerspec cur;
    uint64_t elapsed = timer_get_ticks() - posix_timers[idx].start_tick;
    uint64_t remaining = posix_timers[idx].it_value > elapsed
                         ? posix_timers[idx].it_value - elapsed : 0;

    cur.it_interval.tv_sec  = posix_timers[idx].it_interval / TIMER_FREQ;
    cur.it_interval.tv_nsec = (posix_timers[idx].it_interval % TIMER_FREQ)
                              * (1000000000ULL / TIMER_FREQ);
    cur.it_value.tv_sec     = remaining / TIMER_FREQ;
    cur.it_value.tv_nsec    = (remaining % TIMER_FREQ)
                              * (1000000000ULL / TIMER_FREQ);

    if (copy_to_user(cur_addr, &cur, sizeof(struct itimerspec)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    return 0;
}

/* ── sys_timer_getoverrun ────────────────────────────────────────
 *
 *   timer_getoverrun(timerid)
 *
 * Returns the overrun count (number of extra expirations that
 * occurred between the signal delivery and the timer_getoverrun call).
 */
uint64_t sys_timer_getoverrun(uint64_t timerid)
{
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    return posix_timers[idx].overrun;
}

/* ── sys_timer_delete ────────────────────────────────────────────
 *
 *   timer_delete(timerid)
 *
 * Destroys a POSIX per-process timer, freeing its slot.
 */
uint64_t sys_timer_delete(uint64_t timerid)
{
    int idx = (int)timerid - 1;
    if (idx < 0 || idx >= POSIX_TIMER_MAX || !posix_timers[idx].in_use)
        return (uint64_t)(int64_t)-EINVAL;

    posix_timers[idx].in_use = 0;
    return 0;
}

/* ── posix_timer_init ───────────────────────────────────────────
 *
 * Called during boot from production_subsystems_init() to clear
 * the POSIX timer table.
 */
void posix_timer_init(void)
{
    memset(posix_timers, 0, sizeof(posix_timers));
}

/* ── posix_timer_tick ────────────────────────────────────────────
 *
 * Called from the timer interrupt (timer.c) on every tick.
 * Checks all armed POSIX timers and delivers signals on expiry.
 */
void posix_timer_tick(void)
{
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
