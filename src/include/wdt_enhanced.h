#ifndef WDT_ENHANCED_H
#define WDT_ENHANCED_H

#include "types.h"

/*
 * wdt_enhanced.h — Enhanced watchdog pretimeout NMI and governor support
 *
 * Extends the basic software watchdog with:
 *   - Pretimeout NMI delivery via APIC (lapic_send_ipi with NMI delivery mode)
 *   - Configurable governors: panic, dump, none
 *   - /sys/class/watchdog/watchdog0/ sysfs interface
 *
 * Architecture: x86-64
 */

/* ── Pretimeout governors ──────────────────────────────────────────── */
#define WDT_GOVERNOR_PANIC  0   /* Call panic() on pretimeout */
#define WDT_GOVERNOR_DUMP   1   /* Dump register state of all CPUs, don't reset */
#define WDT_GOVERNOR_NONE   2   /* No action on pretimeout */

/* ── Public API ────────────────────────────────────────────────────── */

/* Set the pretimeout governor (WDT_GOVERNOR_PANIC, _DUMP, or _NONE). */
void wdt_set_pretimeout_governor(int gov);

/* Get the current pretimeout governor. */
int wdt_get_pretimeout_governor(void);

/*
 * Send an NMI to all CPUs (including self) via APIC.
 * Uses the local APIC ICR with delivery mode = NMI (100b).
 * Safe to call with interrupts disabled.
 */
void wdt_send_nmi_all_cpus(void);

/*
 * Dump register state of all CPUs.
 * In SMP mode, sends NMI backtrace IPIs to other CPUs to capture
 * their register state, then dumps the local CPU's registers.
 * In UP mode, just dumps the local CPU.
 */
void wdt_dump_all_cpu_regs(void);

/*
 * Initialize the watchdog sysfs interface.
 * Creates /sys/class/watchdog/ and /sys/class/watchdog/watchdog0/
 * with pretimeout, pretimeout_governor, and timeout attributes.
 * Called once during boot after sysfs_init().
 */
void watchdog_sysfs_init(void);

/*
 * Watchdog pretimeout action — called from the pretimeout timer tick.
 * Evaluates the current governor and takes the configured action:
 *   PANIC → call panic()
 *   DUMP  → print register dumps, do NOT reset
 *   NONE  → print warning, do nothing else
 */
void wdt_pretimeout_action(void);

/* ── Convenience: NMI delivery via APIC ICR ───────────────────────────
 *
 * The APIC ICR delivery mode for NMI is 4 (binary 100).
 * Defined here so both watchdog and other subsystems can use it.
 */
#define ICR_DELIVERY_NMI    (4UL << 8)   /* NMI delivery mode for ICR */

/* NMI vector number used for IPI-based NMI delivery.
 * When the LAPIC ICR delivery mode is NMI, the vector field in
 * bits [7:0] is ignored (NMI has no vector number), but we still
 * need to write a valid-looking value for the hardware.  Using
 * vector 0x02 (the legacy NMI pin vector) is conventional. */
#define IPI_VECTOR_NMI       0x02   /* Legacy NMI vector number */

#endif /* WDT_ENHANCED_H */
