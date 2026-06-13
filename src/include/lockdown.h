#ifndef LOCKDOWN_H
#define LOCKDOWN_H

#include "types.h"

/* ── Lockdown modes ────────────────────────────────────────────────── */

#define LOCKDOWN_NONE             0   /* No lockdown */
#define LOCKDOWN_INTEGRITY        1   /* Block kexec, /dev/mem, hibernation */
#define LOCKDOWN_CONFIDENTIALITY  2   /* Additionally block dmesg, kallsyms */

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * lock_down — Raise the lockdown level.
 * @level: LOCKDOWN_INTEGRITY or LOCKDOWN_CONFIDENTIALITY.
 *
 * Once set, lockdown level can never be lowered.
 */
void lock_down(int level);

/**
 * lockdown_is_locked_down — Check if a given lockdown level is active.
 * @level: Level to check (LOCKDOWN_INTEGRITY or LOCKDOWN_CONFIDENTIALITY).
 *
 * Returns 1 if the system is locked down at or above @level, 0 otherwise.
 */
int lockdown_is_locked_down(int level);

/**
 * lockdown_get_level — Get the current lockdown level.
 *
 * Returns the current lockdown level (LOCKDOWN_NONE, LOCKDOWN_INTEGRITY,
 * or LOCKDOWN_CONFIDENTIALITY).
 */
int lockdown_get_level(void);

#endif /* LOCKDOWN_H */
