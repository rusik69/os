#ifndef NMI_WATCHDOG_H
#define NMI_WATCHDOG_H

#include "types.h"
#include "smp.h"
#include "idt.h"

/*
 * NMI Watchdog — detects kernel hangs via periodic NMI and timer tick
 * monitoring, distinguishing hard lockups (interrupts-off stalls) from
 * soft lockups (scheduler not invoked despite timer ticks).
 *
 * Hard lockup detection:
 *   An NMI fires every HARD_LOCKUP_THRESHOLD_MS (via the local APIC
 *   performance-monitoring counter or LVT).  The NMI handler checks if
 *   the CPU has petted the watchdog (nmi_watchdog_pet()) within the
 *   threshold.  If not, it's a hard lockup — the CPU is stuck with IRQs
 *   disabled for too long.
 *
 * Soft lockup detection:
 *   The timer tick handler calls nmi_watchdog_soft_pet() and then
 *   nmi_watchdog_check_soft().  If the soft-pet timestamp has not been
 *   updated by the scheduler/idle loop within SOFT_LOCKUP_THRESHOLD_MS,
 *   it's a soft lockup — timer IRQs are firing but the scheduler isn't
 *   getting to run the watchdog's pet path.
 *
 * On either lockup detection, the detecting CPU sends an IPI to all other
 * CPUs requesting them to dump their register state and stack backtrace
 * for post-mortem analysis.
 */

/* ── Thresholds ──────────────────────────────────────────────────── */
#define HARD_LOCKUP_THRESHOLD_MS  10000UL  /* 10 seconds */
#define SOFT_LOCKUP_THRESHOLD_MS  20000UL  /* 20 seconds */

/* ── Per-CPU watchdog state ──────────────────────────────────────── */
struct nmi_watchdog_cpu {
    /* Petted by nmi_watchdog_pet() from non-NMI context (idle loop,
     * scheduler, etc.).  The NMI handler reads this to detect hard
     * lockups. */
    volatile uint64_t hard_pet_tick;

    /* Petted by nmi_watchdog_soft_pet() from the scheduler/timer tick.
     * nmi_watchdog_check_soft() compares this to the current tick to
     * detect soft lockups. */
    volatile uint64_t soft_pet_tick;

    /* Set when a lockup is already being reported on this CPU, to
     * prevent recursive floods. */
    volatile int      lockup_active;

    /* Set before entering an idle C-state (HLT/MWAIT), cleared after
     * wake.  The NMI handler checks this flag to avoid false-positive
     * hard lockup reports when a PMC counter overflow fires during
     * wake processing while the hard_pet_tick is still stale from the
     * long idle period. */
    volatile int      idle_in_idle_state;

    /* Lockup event counters (read-only from outside) */
    volatile uint64_t hard_lockup_count;
    volatile uint64_t soft_lockup_count;
    volatile uint64_t nmi_count;
};

/* ── Public API ──────────────────────────────────────────────────── */

/* Pet the watchdog from non-NMI context (idle loop, scheduler finish).
 * Updates both hard_pet and soft_pet timestamps for the current CPU. */
void nmi_watchdog_pet(void);

/* Pet only the soft watchdog from the timer tick handler.
 * Does not update the hard pet timestamp so that the NMI handler can
 * distinguish a CPU that's getting timer IRQs (soft lockup possible)
 * from one that isn't (hard lockup). */
void nmi_watchdog_soft_pet(void);

/* Check for soft lockup on the current CPU.
 * Called from the timer tick handler AFTER nmi_watchdog_soft_pet().
 * If the soft_pet_tick is stale, declares a soft lockup. */
void nmi_watchdog_check_soft(void);

/* Start/stop the watchdog timer mechanism. */
void nmi_watchdog_start(void);
void nmi_watchdog_stop(void);

/* NMI vector #2 handler — called on every NMI.
 * The IDT entry for vector 2 must be registered to call this.
 * Detects hard lockups and triggers IPI backtraces. */
void nmi_watchdog_handler(struct interrupt_frame *frame);

/* Returns 1 if NMI-based watchdog is supported on this hardware. */
int  nmi_watchdog_available(void);

/* Initialize the NMI watchdog subsystem.
 * Registers the NMI handler IDT entry and sets up per-CPU state. */
void nmi_watchdog_init(void);

/* Send an IPI to all other CPUs requesting a backtrace dump.
 * Called when a lockup is detected. */
void nmi_watchdog_request_backtrace(void);

/* Lockup statistics snapshot */
struct nmi_watchdog_stats {
    uint64_t hard_lockups;
    uint64_t soft_lockups;
    uint64_t nmi_count;
};

void nmi_watchdog_get_stats(struct nmi_watchdog_stats *stats);

/* Called by cpuidle before entering an idle C-state (HLT/MWAIT/POLL).
 * Sets a per-CPU flag so the NMI handler knows the CPU is legitimately
 * idle — a stale pet timestamp is not a hard lockup. */
void nmi_watchdog_idle_enter(void);

/* Called by cpuidle after waking from an idle C-state.
 * Clears the per-CPU idle flag so the NMI handler resumes normal
 * hard lockup detection. */
void nmi_watchdog_idle_exit(void);

#endif /* NMI_WATCHDOG_H */
