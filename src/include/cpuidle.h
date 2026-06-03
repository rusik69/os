#ifndef CPUIDLE_H
#define CPUIDLE_H

#include "types.h"

/* ── Forward declarations ────────────────────────────────────────── */

/* Per-CPU idle data (defined below).  Forward-declared so the governor
 * struct can reference it before the full definition. */
struct cpuidle_cpu;

/* ── C-state flags ────────────────────────────────────────────────── */

#define CPUIDLE_FLAG_NONE       0x00
#define CPUIDLE_FLAG_TIMER_STOP 0x01  /* C-state stops the local APIC timer */

/* Maximum number of idle states supported */
#define CPUIDLE_MAX_STATES      8

/* Scheduler hint: disable timer tick during deep idle */
#define CPUIDLE_TICK_DISABLE    1
#define CPUIDLE_TICK_ENABLE     0

/* ── Idle state descriptor ────────────────────────────────────────── */

struct cpuidle_state {
    uint8_t  id;               /* Logical C-state index (1=C1, 2=C2, ...) */
    const char *name;          /* Human-readable name */
    uint32_t latency;          /* Wakeup latency in microseconds */
    uint32_t power;            /* Relative power (lower = less power) */
    uint32_t flags;            /* CPUIDLE_FLAG_* bitmask */
    int      (*enter)(struct cpuidle_state *self);  /* Enter this state */
};

/* ── Governor interface ──────────────────────────────────────────── */

/* A governor selects which idle state to enter based on idle duration
 * prediction, latency constraints, and power/performance trade-offs. */
struct cpuidle_governor {
    const char *name;
    /* Select an idle state index given the per-CPU data and registered states.
     * Returns the index of the selected state (guaranteed valid). */
    int (*select)(struct cpuidle_cpu *cpu_data);
    /* Called on each idle entry before selecting a state, to record the
     * actual idle duration of the previous idle period. */
    void (*record_idle)(struct cpuidle_cpu *cpu_data, uint64_t duration_ticks);
};

/* ── Per-CPU idle data ────────────────────────────────────────────── */

struct cpuidle_cpu {
    uint64_t idle_entries;                     /* Number of times CPU entered idle */
    uint64_t idle_time_ticks;                  /* Total ticks spent idle */
    uint64_t state_entries[CPUIDLE_MAX_STATES]; /* Entries per state */
    uint64_t state_time[CPUIDLE_MAX_STATES];    /* Ticks per state */
    uint8_t  last_state_idx;                   /* Index of last-entered state */
    uint8_t  enabled;                          /* cpuidle enabled on this CPU */
    uint8_t  deepest_state;                    /* Deepest usable state index */

    /* ── Menu governor per-CPU state ──────────────────────────── */
    uint64_t menu_predict_us;        /* Predicted idle duration (us) */
    uint64_t menu_ema_us;            /* Exponential moving average of idle durations (us) */
    uint64_t menu_prev_duration_us;  /* Duration of the last idle period (us) */
    uint32_t menu_correction_factor; /* Correction factor (fixed-point 16.16) */
    uint8_t  menu_voter_initialized; /* Whether EMA has been seeded */
};

/* ── Top-level API ────────────────────────────────────────────────── */

/* Initialise the cpuidle subsystem — detect CPU capabilities */
void cpuidle_init(void);

/* Initialise per-CPU idle data (called on each CPU during AP bringup) */
void cpuidle_init_cpu(void);

/* Enter idle — called when the CPU has no runnable tasks.
 * Selects the deepest available C-state and enters it. */
void cpuidle_idle(void);

/* Disable/enable cpuidle on the current CPU */
void cpuidle_disable(void);
void cpuidle_enable(void);

/* Query number of registered idle states */
int  cpuidle_state_count(void);

/* Retrieve a pointer to the Nth idle state (NULL if out of range) */
const struct cpuidle_state *cpuidle_get_state(int idx);

/* Return cumulative idle statistics for the current CPU */
uint64_t cpuidle_get_idle_entries(void);
uint64_t cpuidle_get_idle_time(void);

/* ── Governor registration ───────────────────────────────────────── */

/* Register a governor for use by the idle loop.  If no governor is
 * explicitly selected, the menu governor is used by default. */
void cpuidle_register_governor(const struct cpuidle_governor *gov);

/* Select which registered governor to use (by name).  Returns 0 on
 * success, -1 if no governor with that name is registered. */
int cpuidle_select_governor(const char *name);

/* ── Architecture-specific state enter functions ──────────────────── */

/* C1 — basic HLT-based idle (always available) */
int cpuidle_c1_halt_enter(struct cpuidle_state *self);

/* C1E — MWAIT-based shallow idle (requires MWAIT support) */
int cpuidle_c1e_mwait_enter(struct cpuidle_state *self);

/* C2 — MWAIT-based deeper idle (requires MWAIT support) */
int cpuidle_c2_mwait_enter(struct cpuidle_state *self);

/* C3 — MWAIT-based deepest idle, may stop timers (requires MWAIT support) */
int cpuidle_c3_mwait_enter(struct cpuidle_state *self);

/* ── ACPI _CST integration (optional, called by acpi_init) ────────── */
int cpuidle_acpi_register_states(uint8_t *cst_data, uint32_t length);

#endif /* CPUIDLE_H */
