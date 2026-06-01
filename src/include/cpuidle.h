#ifndef CPUIDLE_H
#define CPUIDLE_H

#include "types.h"

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

/* ── Per-CPU idle data ────────────────────────────────────────────── */

struct cpuidle_cpu {
    uint64_t idle_entries;                     /* Number of times CPU entered idle */
    uint64_t idle_time_ticks;                  /* Total ticks spent idle */
    uint64_t state_entries[CPUIDLE_MAX_STATES]; /* Entries per state */
    uint64_t state_time[CPUIDLE_MAX_STATES];    /* Ticks per state */
    uint8_t  last_state_idx;                   /* Index of last-entered state */
    uint8_t  enabled;                          /* cpuidle enabled on this CPU */
    uint8_t  deepest_state;                    /* Deepest usable state index */
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
