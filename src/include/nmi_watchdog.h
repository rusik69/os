#ifndef NMI_WATCHDOG_H
#define NMI_WATCHDOG_H

#include "types.h"

/*
 * NMI Watchdog — detects kernel hangs via periodic NMI interrupts.
 *
 * The watchdog programs the local APIC to generate an NMI every
 * WATCHDOG_TIMEOUT_MS milliseconds. If the main kernel loop doesn't
 * "pet" the watchdog by calling nmi_watchdog_pet() within the timeout,
 * the NMI handler dumps debug info (registers, backtrace, task list)
 * and optionally panics.
 */

#define WATCHDOG_TIMEOUT_MS 5000  /* 5 seconds before watchdog barks */

/* Pet the watchdog — call periodically from the idle loop or scheduler */
void nmi_watchdog_pet(void);

/* Start the watchdog timer */
void nmi_watchdog_start(void);

/* Stop the watchdog timer */
void nmi_watchdog_stop(void);

/* NMI handler — called from the IDT entry for NMI vector */
void nmi_watchdog_handler(void);

/* Check if watchdog support is available */
int nmi_watchdog_available(void);

/* Initialize NMI watchdog */
void nmi_watchdog_init(void);

#endif
