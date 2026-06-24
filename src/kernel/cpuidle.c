/*
 * cpuidle.c — CPU idle state management with pluggable governors
 *
 * A production-quality idle subsystem that detects CPU idle capabilities
 * via CPUID, exposes multiple C-states (POLL, C1/HLT, C1E/MWAIT, C2, C3),
 * supports ACPI _CST/LPIT platform-specific states, and uses predictive
 * governors to select the optimal C-state for power savings without
 * sacrificing performance.
 *
 * Governors:
 *   - Menu governor (default): Uses exponential moving average of past
 *     idle durations plus next-timer-event prediction to select the
 *     deepest state whose break-even latency is below the predicted
 *     idle length.  Includes POLL detection for sub-C1-breakeven idles.
 *
 * Design mirrors the Linux cpuidle subsystem at a high level:
 *   - State discovery at boot (CPUID leaf 5 for MWAIT, leaf 1 for HLT)
 *   - Pluggable governor architecture with registration API
 *   - Per-CPU idle data for statistics
 *   - Latency-aware state selection with PM QoS integration
 *   - ACPI _CST/LPIT probe for platform-defined C-states
 */

#define KERNEL_INTERNAL
#include "cpuidle.h"
#include "pm_qos.h"
#include "printf.h"
#include "cpu.h"
#include "smp.h"
#include "timer.h"
#include "string.h"
#include "preempt.h"   /* for need_resched() */
#include "nohz.h"      /* for nohz_tick_stop/nohz_tick_restart */

/* ═══════════════════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════════════════ */

/* Timer frequency in Hz (used for tick-to-us conversion) */
#ifndef TIMER_FREQ
#define TIMER_FREQ 100
#endif

/* Menu governor tuning constants */
#define MENU_EMA_ALPHA_NUM    3   /* EMA numerator   (alpha = 3/8 = 0.375) */
#define MENU_EMA_ALPHA_DENOM  8   /* EMA denominator */
#define MENU_CORRECTION_MAX   0xFFFFULL  /* Max correction factor (16.16 fp) */
#define MENU_CORRECTION_INIT  0x10000UL  /* 1.0 in 16.16 fixed point */
#define MENU_STDDEV_FACTOR    3   /* Weight stddev in prediction */
#define MENU_MIN_IDLE_US      100 /* Minimum idle to consider deeper than C1 (us) */

/* POLL state parameters */
#define POLL_BREAK_EVEN_US    0   /* Polling has zero wakeup cost */
#define POLL_LATENCY_US       0   /* Instant wakeup */
#define POLL_ITER_MAX         500 /* Max PAUSE iterations before yielding */

/* ═══════════════════════════════════════════════════════════════════════
 *  Global state tables — populated at boot
 * ═══════════════════════════════════════════════════════════════════════ */

static struct cpuidle_state idle_states[CPUIDLE_MAX_STATES];
static int idle_state_count = 0;

/* Feature flags detected via CPUID */
static int have_mwait = 0;      /* MWAIT/MONITOR instructions available */
static int have_mwait_ext = 0;  /* Extended MWAIT C-state hints (CPUID.05) */

/* ── Governor state ──────────────────────────────────────────────── */

/* Maximum number of registered governors */
#define MAX_GOVERNORS 4

static const struct cpuidle_governor *governors[MAX_GOVERNORS];
static int governor_count = 0;

/* Currently active governor (default: menu governor, index 0) */
static const struct cpuidle_governor *active_governor = NULL;

/* ═══════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════ */

static int menu_governor_select(struct cpuidle_cpu *cpu_data);
static void menu_governor_record(struct cpuidle_cpu *cpu_data,
                                 uint64_t duration_ticks);

/* Built-in menu governor descriptor */
static const struct cpuidle_governor menu_governor = {
    .name        = "menu",
    .select      = menu_governor_select,
    .record_idle = menu_governor_record,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/* Return a pointer to the current CPU's cpuidle data. */
static inline struct cpuidle_cpu *this_cpu_idle(void)
{
    return &get_cpu_info()->idle_data;
}

/* Check if the current CPU has a pending reschedule request. */
static inline int cpuidle_need_resched(void)
{
    return need_resched();
}

/* Check CPUID for MWAIT support.  Sets have_mwait and have_mwait_ext. */
static void cpuidle_detect_caps(void)
{
    int rax, rbx, rcx, rdx;

    /* CPUID leaf 1 — ECX bit 3 = MONITOR/MWAIT */
    __asm__ volatile("cpuid"
                     : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx)
                     : "a"(1));
    if (rcx & (1U << 3)) {
        have_mwait = 1;
        kprintf("[cpuidle] MWAIT/MONITOR supported\n");
    } else {
        have_mwait = 0;
        kprintf("[cpuidle] MWAIT not available, using HLT only\n");
    }

    /* CPUID leaf 5 — MWAIT extended features */
    if (have_mwait) {
        __asm__ volatile("cpuid"
                         : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx)
                         : "a"(5));
        if (rax & 1) {
            have_mwait_ext = 1;
        }
    }
}

/* ── MWAIT wrapper ────────────────────────────────────────────────── */

/* Execute MONITOR for address tracking, then MWAIT with hint.
 * The hint is: bits [3:0] = C-state sub-state, bits [7:4] = C-state.
 * Returns immediately on wake (store to monitored address or interrupt). */
static inline void
do_mwait(volatile uint64_t *addr, uint32_t hint)
{
    __asm__ volatile(
        "movq %[addr], %%rax\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "monitor\n\t"
        "movq %[hint], %%rax\n\t"
        "xor %%rcx, %%rcx\n\t"
        "mwait\n\t"
        :
        : [addr] "r" (addr), [hint] "r" ((uint64_t)hint)
        : "rax", "rcx", "rdx", "memory");
}

/* MWAIT with hint from ACPI _CST entry param.  The hint encodes the
 * C-state (bits [7:4]) and sub-state (bits [3:0]) in the eax value. */
static inline void
do_mwait_hint(uint64_t hint)
{
    volatile uint64_t monitor_addr = 0;
    __asm__ volatile("sti" : : : "memory");
    do_mwait(&monitor_addr, (uint32_t)hint);
    __asm__ volatile("cli" : : : "memory");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  State enter functions
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Global state ────────────────────────────────────────────────── */
static int g_cpuidle_initialized = 0;
static int cpuidle_global_enabled = 1;
static int g_state_count = 0;

/*
 * POLL idle state — busy-wait with PAUSE instruction.
 *
 * When the predicted idle time is extremely short (below the break-even
 * of even the HLT-based C1), we poll in a tight loop.  This avoids the
 * cost of entering/exiting C1 (TLB flush, interrupt setup, etc.) at the
 * expense of burning power.  The loop periodically checks the
 * TIF_NEED_RESCHED flag so it can exit immediately when work arrives.
 *
 * Used for sub-microsecond to ~10us idle predictions where any C-state
 * entry/exit overhead is not worth the power saving.
 */
int cpuidle_poll_enter(struct cpuidle_state *self)
{
    (void)self;
    int iter = 0;

    /* Spin with PAUSE — checks reschedule flag every few iterations.
     * Interrupts remain enabled so the timer tick is not blocked. */
    __asm__ volatile("sti" : : : "memory");

    while (!cpuidle_need_resched()) {
        __asm__ volatile("pause" : : : "memory");
        iter++;
        /* Yield eagerly if we've been polling for a while without
         * seeing a reschedule — someone else might need this CPU. */
        if (iter >= POLL_ITER_MAX) {
            __asm__ volatile("rep; nop" : : : "memory");
            iter = 0;
        }
    }

    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* C1 — HLT-based idle (always available) */
int cpuidle_c1_halt_enter(struct cpuidle_state *self)
{
    (void)self;
    __asm__ volatile("sti; hlt; cli" : : : "memory");
    return 0;
}

/* MWAIT-based C1E entry (shallow) */
int cpuidle_c1e_mwait_enter(struct cpuidle_state *self)
{
    (void)self;
    volatile uint64_t monitor_addr = 0;
    __asm__ volatile("sti" : : : "memory");
    /* C1E hint: sub-state 1, C-state 1 */
    do_mwait(&monitor_addr, 0x11);
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* MWAIT-based C2 entry (medium) */
int cpuidle_c2_mwait_enter(struct cpuidle_state *self)
{
    (void)self;
    volatile uint64_t monitor_addr = 0;
    __asm__ volatile("sti" : : : "memory");
    /* C2 hint: sub-state 0, C-state 2 */
    do_mwait(&monitor_addr, 0x20);
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* MWAIT-based C3 entry (deep) */
int cpuidle_c3_mwait_enter(struct cpuidle_state *self)
{
    (void)self;
    volatile uint64_t monitor_addr = 0;
    __asm__ volatile("sti" : : : "memory");
    /* C3 hint: sub-state 0, C-state 3 */
    do_mwait(&monitor_addr, 0x30);
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/*
 * Generic MWAIT enter function for ACPI-registered states.
 * Uses the driver_data field to store the MWAIT hint.
 */
static int cpuidle_mwait_hint_enter(struct cpuidle_state *self)
{
    if (!self || !self->driver_data)
        return -EINVAL;
    uint64_t hint = (uint64_t)(uintptr_t)self->driver_data;
    do_mwait_hint(hint);
    return 0;
}

/*
 * SystemIO-based C-state entry — writes the entry command to an IO port.
 * The driver_data holds the IO port address (lower 32 bits) and value
 * (upper 32 bits).
 */
static int cpuidle_io_enter(struct cpuidle_state *self)
{
    if (!self || !self->driver_data)
        return -EINVAL;
    uint64_t data = (uint64_t)(uintptr_t)self->driver_data;
    uint16_t port = (uint16_t)(data & 0xFFFF);
    uint8_t  value = (uint8_t)((data >> 16) & 0xFF);

    __asm__ volatile("sti" : : : "memory");
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
    /* Wake comes via interrupt; re-disable interrupts on return */
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Menu governor — idle duration prediction and state selection
 * ═══════════════════════════════════════════════════════════════════════
 *
 * The menu governor predicts how long the CPU will stay idle and selects
 * the deepest C-state whose break-even time is within the prediction.
 * For very short predictions where even C1 (HLT) is too expensive, the
 * POLL state is selected instead.
 *
 * Prediction components:
 *   1. Exponential moving average (EMA) of past idle durations
 *   2. Standard deviation of idle durations (for safety margin)
 *   3. Next timer event (upper bound on idle time)
 *
 * The corrected prediction = min(EMA + stddev * factor, next_timer_interval)
 * with an additional correction factor that adapts based on over/under
 * prediction errors.
 */

/* Ticks-to-microseconds conversion (fixed-point to avoid float) */
static inline uint64_t ticks_to_us(uint64_t ticks)
{
    /* TIMER_FREQ ticks/sec = microseconds = ticks * 1,000,000 / TIMER_FREQ */
    return ticks * 1000000ULL / TIMER_FREQ;
}

/*
 * Compute the exponential moving average of idle durations.
 *   ema = ema + alpha * (sample - ema)
 * where alpha = MENU_EMA_ALPHA_NUM / MENU_EMA_ALPHA_DENOM (3/8 = 0.375).
 */
static inline uint64_t menu_ema_update(uint64_t ema, uint64_t sample)
{
    /* Fixed-point arithmetic: ema and sample are in microseconds */
    int64_t diff = (int64_t)sample - (int64_t)ema;
    int64_t delta = (diff * MENU_EMA_ALPHA_NUM) / MENU_EMA_ALPHA_DENOM;
    return ema + (uint64_t)(delta >= 0 ? delta : -delta);
}

/*
 * Estimate the standard deviation of idle durations from the EMA.
 * We approximate stddev as the absolute deviation |sample - ema| * factor.
 */
static inline uint64_t menu_estimate_stddev(uint64_t ema, uint64_t sample)
{
    int64_t dev = (int64_t)sample - (int64_t)ema;
    return (uint64_t)(dev >= 0 ? dev : -dev);
}

/*
 * Record the actual idle duration after waking from idle.
 * Updates the EMA and correction factor based on prediction error.
 */
static void menu_governor_record(struct cpuidle_cpu *cpu_data,
                                 uint64_t duration_ticks)
{
    uint64_t duration_us = ticks_to_us(duration_ticks);
    uint64_t predicted_us = cpu_data->menu_predict_us;

    /* Update exponential moving average */
    if (cpu_data->menu_voter_initialized) {
        cpu_data->menu_ema_us = menu_ema_update(cpu_data->menu_ema_us,
                                                  duration_us);
    } else {
        /* Seed EMA with the first measured duration */
        cpu_data->menu_ema_us = duration_us;
        cpu_data->menu_voter_initialized = 1;
    }

    /* Update correction factor based on prediction error.
     * If we over-predicted (predicted >> actual), the factor increases
     * (more conservative = choose shallower state).  If we under-predicted,
     * the factor decreases (more aggressive = choose deeper state).
     * Use 16.16 fixed point: 1.0 = 0x10000. */
    if (predicted_us > 0 && duration_us > 0) {
        uint64_t ratio = (duration_us * 0x10000ULL) / predicted_us;
        /* Clamp ratio to reasonable range [0.25, 4.0] to avoid extreme swings */
        if (ratio < 0x4000) ratio = 0x4000;   /* 0.25 */
        if (ratio > 0x40000) ratio = 0x40000;  /* 4.0  */

        /* Smooth the correction factor (IIR) */
        uint64_t current = cpu_data->menu_correction_factor;
        if (current == 0)
            current = MENU_CORRECTION_INIT;
        /* new = current * 0.75 + ratio * 0.25 */
        cpu_data->menu_correction_factor =
            (uint32_t)((current * 3 + ratio) >> 2);
        if (cpu_data->menu_correction_factor > MENU_CORRECTION_MAX)
            cpu_data->menu_correction_factor = MENU_CORRECTION_MAX;
        if (cpu_data->menu_correction_factor == 0)
            cpu_data->menu_correction_factor = 1;
    }

    cpu_data->menu_prev_duration_us = duration_us;
}

/*
 * Calculate the predicted idle duration for the upcoming idle period.
 * Combines EMA, standard-deviation safety margin, and the next-timer
 * bound.  The prediction is clamped to the timer interval (we can't
 * idle longer than the next timer tick).
 */
static uint64_t menu_predict_idle_us(struct cpuidle_cpu *cpu_data)
{
    uint64_t ema_us = cpu_data->menu_ema_us;
    uint64_t prev_us = cpu_data->menu_prev_duration_us;

    if (!cpu_data->menu_voter_initialized) {
        /* Not enough data yet — be conservative, assume short idle */
        return MENU_MIN_IDLE_US * 10;
    }

    /* Estimate stddev from the last sample vs EMA */
    uint64_t stddev = menu_estimate_stddev(ema_us, prev_us);

    /* Prediction = EMA + stddev * factor, clamped below */
    uint64_t prediction = ema_us + stddev * MENU_STDDEV_FACTOR;
    if (prediction < MENU_MIN_IDLE_US)
        prediction = MENU_MIN_IDLE_US;

    /* Apply correction factor (16.16 fixed point) */
    uint32_t cf = cpu_data->menu_correction_factor;
    if (cf == 0) cf = MENU_CORRECTION_INIT;
    prediction = (prediction * cf) >> 16;

    /* Bound by the next timer event as the absolute maximum.
     * We approximate the next timer tick by TIMER_FREQ ticks/sec.
     * The actual timer callback fires at most 1/FREQ seconds from now.
     * Use 2 ticks as a reasonable conservative bound (20ms at 100Hz). */
    uint64_t timer_bound_us = ticks_to_us(2);
    if (prediction > timer_bound_us)
        prediction = timer_bound_us;

    return prediction;
}

/*
 * Menu governor state selection.
 * Scans C-states from deepest to shallowest and selects the first
 * whose break-even latency is below the predicted idle duration
 * and that meets PM QoS latency constraints.
 *
 * Special case: if the predicted idle time is less than the C1 (HLT)
 * break-even, we select the POLL state to avoid the overhead of
 * C-state entry/exit for extremely short idle periods.
 */
static int menu_governor_select(struct cpuidle_cpu *cpu_data)
{
    uint32_t max_latency = pm_qos_read_effective_latency();

    /* Predict how long we expect to be idle */
    cpu_data->menu_predict_us = menu_predict_idle_us(cpu_data);
    uint64_t predicted_us = cpu_data->menu_predict_us;

    /* Check if POLL state (index 0) exists and is appropriate.
     * POLL is used when predicted idle is so short that even C1 (HLT)
     * overhead is not worth it. */
    if (idle_state_count > 0 && (idle_states[0].flags & CPUIDLE_FLAG_POLL)) {
        /* C1 break-even: latency * 2 + 10us.  If prediction is below
         * this, we poll instead.  This avoids the latency of C1 entry
         * and the TLB/scheduler overhead. */
        uint64_t c1_break_even = 2 + 2 + 10; /* ~14us minimum for C1 */
        if (predicted_us < c1_break_even) {
            return 0; /* POLL state index */
        }
    }

    /* Scan from deepest to shallowest (skip POLL at index 0 for
     * deeper state selection since we already handled it above) */
    for (int idx = idle_state_count - 1; idx >= 1; idx--) {
        struct cpuidle_state *s = &idle_states[idx];

        /* Check PM QoS latency constraint */
        if (max_latency != PM_QOS_NO_CONSTRAINT &&
            s->latency > max_latency) {
            continue;
        }

        /* Calculate break-even: we must stay idle long enough to
         * amortize the cost of entering and exiting the C-state.
         * A reasonable approximation is 2x the wakeup latency plus
         * a small fixed overhead (10 us for state save/restore). */
        uint64_t break_even_us = (uint64_t)s->latency * 2 + 10;

        /* For C1 (HLT), the break-even is nearly zero — always qualifies */
        if (idx == 1)
            return idx;

        /* Only select a deeper state if the predicted idle duration
         * comfortably exceeds the break-even threshold.  We require
         * at least 1.5x margin to avoid thrashing. */
        if (predicted_us > break_even_us + break_even_us / 2)
            return idx;
    }

    /* Fall back to C1 (HLT) at index 1, or POLL if only POLL exists */
    return (idle_state_count > 1) ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Governor registration & selection
 * ═══════════════════════════════════════════════════════════════════════ */

void cpuidle_register_governor(const struct cpuidle_governor *gov)
{
    if (!gov || !gov->name || !gov->select)
        return;
    if (governor_count >= MAX_GOVERNORS)
        return;

    governors[governor_count++] = gov;

    /* If no governor is active yet, set this one */
    if (active_governor == NULL)
        active_governor = gov;
}

int cpuidle_select_governor(const char *name)
{
    if (!name)
        return -EINVAL;

    for (int i = 0; i < governor_count; i++) {
        if (strcmp(governors[i]->name, name) == 0) {
            active_governor = governors[i];
            kprintf("[cpuidle] Switched to '%s' governor\n", name);
            return 0;
        }
    }
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Built-in state registration
 * ═══════════════════════════════════════════════════════════════════════ */

/* Add a state to the idle_states table.  Returns 0 on success, -1 if full. */
static int cpuidle_register_state(uint8_t id, const char *name,
                                   uint32_t latency, uint32_t power,
                                   uint32_t flags,
                                   int (*enter)(struct cpuidle_state *))
{
    if (idle_state_count >= CPUIDLE_MAX_STATES)
        return -ENOSPC;

    struct cpuidle_state *s = &idle_states[idle_state_count];
    s->id      = id;
    s->name    = name;
    s->latency = latency;
    s->power   = power;
    s->flags   = flags;
    s->enter   = enter;
    s->driver_data = NULL;
    idle_state_count++;
    return 0;
}

/* Register the standard x86-64 idle states based on detected capabilities. */
static void cpuidle_register_builtin(void)
{
    /* POLL — always available, index 0.
     * Zero latency, zero power saving, used for sub-C1-breakeven idles. */
    cpuidle_register_state(0, "POLL",    0,   255, CPUIDLE_FLAG_POLL,
                           cpuidle_poll_enter);

    /* C1 (HLT) — always available */
    cpuidle_register_state(1, "C1 (HLT)",    2,  100, CPUIDLE_FLAG_NONE,
                           cpuidle_c1_halt_enter);

    if (have_mwait) {
        /* C1E — MWAIT shallow */
        cpuidle_register_state(1, "C1E (MWAIT)", 3,  80, CPUIDLE_FLAG_NONE,
                               cpuidle_c1e_mwait_enter);
        /* C2 — MWAIT medium */
        cpuidle_register_state(2, "C2 (MWAIT)",  10, 50, CPUIDLE_FLAG_NONE,
                               cpuidle_c2_mwait_enter);
        /* C3 — MWAIT deep (may stop timer depending on platform) */
        cpuidle_register_state(3, "C3 (MWAIT)",  40, 20, CPUIDLE_FLAG_TIMER_STOP,
                               cpuidle_c3_mwait_enter);
    }

    kprintf("[cpuidle] Registered %d idle states (index 0=POLL, deepest: %s)\n",
            idle_state_count,
            idle_state_count > 0 ? idle_states[idle_state_count - 1].name : "none");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ACPI _CST / LPIT state registration
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Register a custom C-state with cpuidle from a platform driver.
 * Returns the state index on success, -1 on failure.
 */
int cpuidle_register_custom_state(const char *name, uint32_t latency,
                                   uint32_t power, uint32_t flags,
                                   int (*enter)(struct cpuidle_state *))
{
    if (!name || !enter)
        return -EINVAL;
    if (idle_state_count >= CPUIDLE_MAX_STATES)
        return -ENOSPC;

    struct cpuidle_state *s = &idle_states[idle_state_count];
    s->name = name;
    s->latency = latency;
    s->power = power;
    s->flags = flags;
    s->enter = enter;
    s->driver_data = NULL;
    s->id = (uint8_t)(idle_state_count); /* Use index as logical id */
    int idx = idle_state_count;
    idle_state_count++;
    return idx;
}

/*
 * Register platform-specific C-states from ACPI _CST or LPIT data.
 *
 * For FFH (MWAIT) entries, we create states that use the MWAIT hint
 * from the ACPI data.  For SystemIO entries, we create states that
 * write to the specified IO port.
 *
 * Returns the number of states successfully registered, or -1 on error.
 */
int cpuidle_acpi_register_states(const struct acpi_cstate_desc *descs, int count)
{
    /* Static name buffers — one per possible ACPI state, reused across calls.
     * This avoids dynamic allocation and dangling-pointer issues. */
    static char acpi_names[CPUIDLE_MAX_STATES][32];
    static int next_name_slot = 0;

    if (!descs || count <= 0)
        return -EINVAL;

    int registered = 0;

    for (int i = 0; i < count; i++) {
        const struct acpi_cstate_desc *d = &descs[i];

        if (idle_state_count >= CPUIDLE_MAX_STATES) {
            kprintf("[cpuidle] ACPI: no room for more states (max %d)\n",
                    CPUIDLE_MAX_STATES);
            break;
        }

        /* Determine the entry function and driver data based on method */
        int (*enter_fn)(struct cpuidle_state *);
        void *driver_data;

        switch (d->entry_method) {
        case ACPI_CSTATE_ENTRY_FFH:
            /* FFH — Functional Fixed Hardware; entry_param is MWAIT hint */
            enter_fn = cpuidle_mwait_hint_enter;
            driver_data = (void *)(uintptr_t)d->entry_param;
            break;

        case ACPI_CSTATE_ENTRY_IO:
            /* SystemIO — write value to port */
            enter_fn = cpuidle_io_enter;
            /* Pack port (lower 16 bits) and value (next 8 bits) into driver_data */
            driver_data = (void *)(uintptr_t)
                ((d->entry_param & 0xFFFF) | ((uint64_t)(d->entry_param2 & 0xFF) << 16));
            break;

        default:
            kprintf("[cpuidle] ACPI: unknown entry method %d for C-state %d, "
                    "skipping\n", d->entry_method, i);
            continue;
        }

        /* Build a persistent name for the state using a static buffer slot */
        int name_slot = next_name_slot % CPUIDLE_MAX_STATES;
        next_name_slot++;
        const char *method_str = (d->entry_method == ACPI_CSTATE_ENTRY_FFH)
            ? "MWAIT" : "IO";
        uint32_t cstate_num = (d->entry_method == ACPI_CSTATE_ENTRY_FFH)
            ? (uint32_t)((d->entry_param >> 4) & 0xF) : (uint32_t)(i + 2);
        snprintf(acpi_names[name_slot], sizeof(acpi_names[name_slot]),
                 "ACPI-C%u (%s)", cstate_num, method_str);

        /* Register the state */
        struct cpuidle_state *s = &idle_states[idle_state_count];
        s->id      = (uint8_t)(idle_state_count);
        s->name    = acpi_names[name_slot];
        s->latency = d->latency;
        s->power   = (d->latency > 0) ? (100 / d->latency) : 100;
        if (s->power < 1) s->power = 1;
        if (s->power > 100) s->power = 100;
        s->flags   = CPUIDLE_FLAG_NONE;
        s->enter   = enter_fn;
        s->driver_data = driver_data;
        idle_state_count++;
        registered++;

        kprintf("[cpuidle] Registered ACPI C-state %d: lat=%u us res=%u us "
                "method=%s hint/port=0x%llx\n",
                idle_state_count - 1,
                (unsigned int)d->latency,
                (unsigned int)d->residency,
                (d->entry_method == ACPI_CSTATE_ENTRY_FFH) ? "FFH/MWAIT" : "SystemIO",
                (unsigned long long)d->entry_param);

        /* Update deepest state for all CPUs */
        for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
            struct cpu_info *ci = &cpu_info_array[cpu];
            if (idle_state_count - 1 > ci->idle_data.deepest_state)
                ci->idle_data.deepest_state = (uint8_t)(idle_state_count - 1);
        }
    }

    if (registered > 0) {
        kprintf("[cpuidle] Registered %d ACPI C-states (total: %d)\n",
                registered, idle_state_count);
    }

    return registered;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void cpuidle_init(void)
{
    /* Detect CPU idle capabilities */
    cpuidle_detect_caps();

    /* Register built-in idle states (POLL, C1, C1E/C2/C3 if MWAIT) */
    cpuidle_register_builtin();

    /* Register the built-in menu governor */
    cpuidle_register_governor(&menu_governor);

    kprintf("[cpuidle] Initialised (%s governor active, %d states)\n",
            active_governor ? active_governor->name : "none",
            idle_state_count);
}

void cpuidle_init_cpu(void)
{
    struct cpuidle_cpu *c = this_cpu_idle();
    memset(c, 0, sizeof(*c));
    c->enabled       = 1;
    c->deepest_state = (uint8_t)(idle_state_count > 0 ? idle_state_count - 1 : 0);
    c->last_state_idx = 0;
    c->menu_correction_factor = MENU_CORRECTION_INIT;
}

void cpuidle_idle(void)
{
    struct cpu_info *ci = get_cpu_info();
    struct cpuidle_cpu *c = &ci->idle_data;

    if (!c->enabled) {
        /* cpuidle disabled — use plain HLT as fallback */
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        return;
    }

    /* Pet the watchdog — even in idle, prove forward progress */
    extern void nmi_watchdog_pet(void);
    nmi_watchdog_pet();

    /* Use the active governor to select a C-state */
    int state_idx = 0; /* default: POLL */
    if (active_governor && active_governor->select) {
        state_idx = active_governor->select(c);
    }

    /* Clamp to valid range */
    if (state_idx > c->deepest_state)
        state_idx = c->deepest_state;
    if (state_idx < 0 || state_idx >= idle_state_count)
        state_idx = 0; /* Fall back to POLL */

    /* Double-check bounds against cpu_data->deepest_state before array access */
    if ((uint8_t)state_idx > c->deepest_state || state_idx >= CPUIDLE_MAX_STATES)
        state_idx = 0;

    struct cpuidle_state *state = &idle_states[state_idx];

    /* ── NO_HZ: stop the periodic tick before entering deep idle ── */
    int tick_stopped = 0;
    if (nohz_cpu_is_isolated((int)ci->cpu_id) && state_idx > 0) {
        if (nohz_tick_stop((int)ci->cpu_id) == 0)
            tick_stopped = 1;
    }

    /* Record entry — note start tick for the governor's record callback */
    uint64_t start = timer_get_ticks();
    c->state_entries[state_idx]++;
    c->last_state_idx = (uint8_t)state_idx;

    /* Enter the idle state (interrupts enabled inside, disabled on return) */
    state->enter(state);

    /* Compute time spent in ticks */
    uint64_t elapsed = timer_get_ticks() - start;

    /* ── NO_HZ: restart the tick after waking ──────────────────── */
    if (tick_stopped) {
        nohz_tick_restart((int)ci->cpu_id);
    }

    /* Record the actual idle duration so the governor can learn */
    if (active_governor && active_governor->record_idle) {
        active_governor->record_idle(c, elapsed);
    }

    c->state_time[state_idx] += elapsed;
    c->idle_time_ticks += elapsed;
    c->idle_entries++;
}

void cpuidle_disable(void)
{
    struct cpuidle_cpu *c = this_cpu_idle();
    c->enabled = 0;
}

void cpuidle_enable(void)
{
    if (!g_cpuidle_initialized)
        return;
    cpuidle_global_enabled = 1;
}

int cpuidle_state_count(void)
{
    return g_state_count;
}

/* ── Stub: cpuidle_select ──────────────────────────────────────────── */
int cpuidle_select(void *dev)
{
    (void)dev;
    kprintf("[CPUIDLE] cpuidle_select: not yet implemented\n");
    return 0;
}

/* ── Stub: cpuidle_enter ───────────────────────────────────────────── */
int cpuidle_enter(void *dev, int state_index)
{
    (void)dev; (void)state_index;
    kprintf("[CPUIDLE] cpuidle_enter: not yet implemented\n");
    return 0;
}

/* ── Stub: cpuidle_reflect ─────────────────────────────────────────── */
void cpuidle_reflect(void *dev, int state_index)
{
    (void)dev; (void)state_index;
    kprintf("[CPUIDLE] cpuidle_reflect: not yet implemented\n");
}

const struct cpuidle_state *cpuidle_get_state(int idx)
{
    if (idx < 0 || idx >= idle_state_count)
        return NULL;
    return &idle_states[idx];
}

uint64_t cpuidle_get_idle_entries(void)
{
    struct cpuidle_cpu *c = this_cpu_idle();
    return c->idle_entries;
}

uint64_t cpuidle_get_idle_time(void)
{
    struct cpuidle_cpu *c = this_cpu_idle();
    return c->idle_time_ticks;
}
