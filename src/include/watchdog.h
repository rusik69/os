#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "types.h"

/* Pretimeout callback type */
typedef void (*watchdog_pretimeout_fn_t)(void);

/* Initialize the software watchdog with the given timeout in seconds.
 * The watchdog will reboot the system if watchdog_pet() is not called
 * within the timeout period. */
void watchdog_init(int timeout_seconds);

/* Pet (reset) the watchdog timer — call this periodically to prevent reboot. */
void watchdog_pet(void);

/* Stop/cancel the watchdog. */
void watchdog_stop(void);

/* ── Pretimeout support ────────────────────────────────────────────── */

/* Set a pretimeout in seconds: a callback will fire this many seconds
   before the actual watchdog timeout. Set to 0 to disable.
   The pretimeout callback fires only once per timeout cycle. */
void watchdog_set_pretimeout(int secs);

/* Set the function to be called when the pretimeout fires.
   Pass NULL to clear the callback. */
void watchdog_set_pretimeout_fn(watchdog_pretimeout_fn_t fn);

/* ── System reset (usable from interrupt-disabled panic context) ──── */

/*
 * Attempt to reset the machine using multiple fallback methods:
 *   1. ACPI reset register (FADT RESET_REG)
 *   2. Keyboard controller (port 0x64, 0xFE)
 *   3. Legacy reset ports (0x604 BX_RST, 0xB004)
 *   4. Triple-fault via zero-IDT trick
 *
 * This function never returns — if all reset methods fail it halts.
 * Safe to call with interrupts disabled.
 */
__attribute__((noreturn))
void watchdog_system_reset(void);

#endif /* WATCHDOG_H */
