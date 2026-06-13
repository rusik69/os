/*
 * lockdown.c — Kernel lockdown mode
 *
 * Provides two lockdown levels:
 *   LOCKDOWN_INTEGRITY       — Block kexec, /dev/mem, hibernation
 *   LOCKDOWN_CONFIDENTIALITY — Additionally block dmesg, kallsyms
 *
 * Once raised, the lockdown level can never be reduced.
 */

#include "lockdown.h"
#include "printf.h"
#include "string.h"

/* ── State ──────────────────────────────────────────────────────────── */

/* Current lockdown level: starts at LOCKDOWN_NONE, can only increase. */
static volatile int g_lockdown_level = LOCKDOWN_NONE;

/* ── Public API ─────────────────────────────────────────────────────── */

void lock_down(int level)
{
    if (level < LOCKDOWN_NONE || level > LOCKDOWN_CONFIDENTIALITY) {
        kprintf("[lockdown] Invalid level %d, ignoring\n", level);
        return;
    }

    /* Lockdown is irreversible — only allow raising the level */
    if (level <= g_lockdown_level) {
        kprintf("[lockdown] Already at level %d, cannot lower to %d\n",
                g_lockdown_level, level);
        return;
    }

    const char *name = "none";
    if (level == LOCKDOWN_INTEGRITY)
        name = "integrity";
    else if (level == LOCKDOWN_CONFIDENTIALITY)
        name = "confidentiality";

    g_lockdown_level = level;
    kprintf("[lockdown] Kernel locked down to %s mode (level %d)\n", name, level);
}

int lockdown_is_locked_down(int level)
{
    if (level == LOCKDOWN_INTEGRITY || level == LOCKDOWN_CONFIDENTIALITY) {
        return g_lockdown_level >= level ? 1 : 0;
    }
    return 0;
}

int lockdown_get_level(void)
{
    return g_lockdown_level;
}
