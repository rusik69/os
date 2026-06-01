/*
 * cpuidle.c — CPU idle state management
 *
 * A production-quality idle subsystem that detects CPU idle capabilities
 * via CPUID, exposes multiple C-states (C1/HLT, C1E/MWAIT, C2, C3), and
 * selects the deepest available state when the CPU is idle.  Tracks
 * per-CPU statistics for power-management monitoring.
 *
 * Design mirrors the Linux cpuidle subsystem at a high level:
 *   - State discovery at boot (CPUID leaf 5 for MWAIT, leaf 1 for HLT)
 *   - Per-CPU idle data for statistics
 *   - Latency-aware state selection (future: PM QoS integration)
 *   - ACPI _CST probe for platform-defined C-states
 */

#include "cpuidle.h"
#include "printf.h"
#include "cpu.h"
#include "smp.h"
#include "timer.h"
#include "string.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  Global idle state table — populated at boot
 * ═══════════════════════════════════════════════════════════════════════ */

/* Statically defined idle states; the 'enter' pointers are set at init. */
static struct cpuidle_state idle_states[CPUIDLE_MAX_STATES];
static int idle_state_count = 0;

/* Feature flags detected via CPUID */
static int have_mwait = 0;   /* MWAIT/MONITOR instructions available */
static int have_mwait_ext = 0; /* Extended MWAIT C-state hints (CPUID.05) */

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/* Return a pointer to the current CPU's cpuidle data.
 * Zero-initialised by BSS; the data lives in the cpu_info struct. */
static inline struct cpuidle_cpu *this_cpu_idle(void)
{
    return &get_cpu_info()->idle_data;
}

/* Check CPUID for MWAIT support.  Sets have_mwait and have_mwait_ext. */
static void cpuidle_detect_caps(void)
{
    int rax, rbx, rcx, rdx;

    /* CPUID leaf 1 — ECX bit 3 = MONITOR/MWAIT */
    __asm__ volatile("cpuid"
                     : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx)
                     : "a"(1));
    if (rcx & (1 << 3)) {
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
static void __attribute__((always_inline))
do_mwait(volatile uint64_t *addr, uint32_t hint)
{
    /* MONITOR: addr in RAX, extensions in RCX, hints in RDX.
     * MWAIT: C-state hint in RAX, extensions in RCX.
     * Both put RAX to use, so we must load addr into RAX for MONITOR
     * and hint into RAX for MWAIT — they cannot be the same register.
     * Workaround: move addr into RAX for MONITOR, then move hint into
     * RAX for MWAIT (both are simple moves, no memory ref needed). */
    __asm__ volatile(
        "movq  %[addr], %%rax\n\t"
        "monitor\n\t"
        "movl  %[hint], %%eax\n\t"
        "mwait\n\t"
        :
        : [addr] "r"(addr), [hint] "r"(hint)
        : "rax", "rcx", "rdx", "memory"
    );
}

/* ═══════════════════════════════════════════════════════════════════════
 *  State enter functions
 * ═══════════════════════════════════════════════════════════════════════ */

/* C1 — HLT instruction (always available) */
int cpuidle_c1_halt_enter(struct cpuidle_state *self)
{
    (void)self;

    /* Ensure interrupts are enabled so HLT wakes on any interrupt.
     * schedule() disables interrupts before calling us; we re-enable
     * before HLT and they are disabled again on wake. */
    __asm__ volatile("sti; hlt; cli" : : : "memory");
    return 0;
}

/* C1E — MWAIT(C1) — slightly lower power than HLT on some CPUs */
int cpuidle_c1e_mwait_enter(struct cpuidle_state *self)
{
    (void)self;

    if (!have_mwait) {
        /* Fall back to HLT if MWAIT disappeared (race with CPU hotplug?) */
        return cpuidle_c1_halt_enter(self);
    }

    /* Use a per-CPU variable as the monitor target.  Any IPI or store
     * to this address will wake the CPU. */
    volatile uint64_t *monitor_addr = &get_cpu_info()->idle_data.idle_entries;

    __asm__ volatile("sti" : : : "memory");
    do_mwait(monitor_addr, 0x00); /* hint 0 = C1 */
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* C2 — MWAIT(C2) — deeper idle with higher wakeup latency */
int cpuidle_c2_mwait_enter(struct cpuidle_state *self)
{
    (void)self;

    if (!have_mwait) {
        return cpuidle_c1_halt_enter(self);
    }

    volatile uint64_t *monitor_addr = &get_cpu_info()->idle_data.idle_entries;

    __asm__ volatile("sti" : : : "memory");
    /* Hint 0x10 = C2 (bits [7:4] = 0001) */
    do_mwait(monitor_addr, 0x10);
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* C3 — MWAIT(C3) — deepest MWAIT idle, may stop local timer */
int cpuidle_c3_mwait_enter(struct cpuidle_state *self)
{
    (void)self;

    if (!have_mwait) {
        return cpuidle_c1_halt_enter(self);
    }

    volatile uint64_t *monitor_addr = &get_cpu_info()->idle_data.idle_entries;

    __asm__ volatile("sti" : : : "memory");
    /* Hint 0x20 = C3 (bits [7:4] = 0010) */
    do_mwait(monitor_addr, 0x20);
    __asm__ volatile("cli" : : : "memory");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  State selection heuristic
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Select the deepest usable idle state for the current CPU.
 *
 * Currently we prefer deepest because the scheduler has already confirmed
 * there are no runnable tasks.  In the future this should consider:
 *   - PM QoS latency constraints from drivers
 *   - Predicted idle duration (if the CPU has been idle for a while,
 *     we can go deeper)
 *   - Whether the local timer stops (C3 on some systems)
 */
static int cpuidle_select_state(struct cpuidle_cpu *cpu_data)
{
    (void)cpu_data;

    /* Go to deepest available state.  The state table is ordered
     * from shallowest (idx 0) to deepest (idx n-1). */
    int idx = idle_state_count - 1;
    if (idx < 0)
        return 0; /* Should never happen (we always have C1) */

    return idx;
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
        return -1;

    struct cpuidle_state *s = &idle_states[idle_state_count];
    s->id      = id;
    s->name    = name;
    s->latency = latency;
    s->power   = power;
    s->flags   = flags;
    s->enter   = enter;
    idle_state_count++;
    return 0;
}

/* Register the standard x86-64 idle states based on detected capabilities. */
static void cpuidle_register_builtin(void)
{
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

    kprintf("[cpuidle] Registered %d idle states (deepest: %s)\n",
            idle_state_count,
            idle_state_count > 0 ? idle_states[idle_state_count - 1].name : "none");
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

void cpuidle_init(void)
{
    /* Detect CPU idle capabilities */
    cpuidle_detect_caps();

    /* Register built-in idle states */
    cpuidle_register_builtin();

    /* Initialise BSP's per-CPU data */
    cpuidle_init_cpu();

    kprintf("[cpuidle] Initialised\n");
}

void cpuidle_init_cpu(void)
{
    struct cpuidle_cpu *c = this_cpu_idle();
    memset(c, 0, sizeof(*c));
    c->enabled       = 1;
    c->deepest_state = (uint8_t)(idle_state_count > 0 ? idle_state_count - 1 : 0);
    c->last_state_idx = 0;
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

    /* Select the appropriate C-state */
    int state_idx = cpuidle_select_state(c);

    /* Clamp to deepest usable */
    if (state_idx > c->deepest_state)
        state_idx = c->deepest_state;
    if (state_idx < 0 || state_idx >= idle_state_count)
        state_idx = 0; /* Fall back to C1 */

    struct cpuidle_state *state = &idle_states[state_idx];

    /* Record entry */
    uint64_t start = timer_get_ticks();
    c->state_entries[state_idx]++;
    c->last_state_idx = (uint8_t)state_idx;

    /* Enter the idle state (interrupts enabled inside, disabled on return) */
    state->enter(state);

    /* Record exit — compute time spent in ticks */
    uint64_t elapsed = timer_get_ticks() - start;
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
    struct cpuidle_cpu *c = this_cpu_idle();
    c->enabled = 1;
}

int cpuidle_state_count(void)
{
    return idle_state_count;
}

const struct cpuidle_state *cpuidle_get_state(int idx)
{
    if (idx < 0 || idx >= idle_state_count)
        return NULL;
    return &idle_states[idx];
}

uint64_t cpuidle_get_idle_entries(void)
{
    return this_cpu_idle()->idle_entries;
}

uint64_t cpuidle_get_idle_time(void)
{
    return this_cpu_idle()->idle_time_ticks;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ACPI _CST integration stub
 * ═══════════════════════════════════════════════════════════════════════ */

int cpuidle_acpi_register_states(uint8_t *cst_data, uint32_t length)
{
    (void)cst_data;
    (void)length;

    /*
     * Future: parse the _CST package (ACPI 6.5, section 8.1.3).
     * Each entry contains:
     *   - Register (GAS) for the C-state entry method
     *   - Type (1= C1, 2= C2, 3= C3)
     *   - Latency (microseconds)
     *   - Power (milliwatts)
     *
     * For now, the built-in states (HLT/MWAIT) serve as a reasonable
     * default.  ACPI _CST can override them on platforms that support
     * deeper C-states via PM1/PM2 I/O ports.
     */
    return 0;
}
