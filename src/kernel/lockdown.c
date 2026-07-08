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
        return g_lockdown_level >= level;
    }
    return 0;
}

int lockdown_get_level(void)
{
    return g_lockdown_level;
}

/* ── lockdown_set ────────────────────────────────────────────── */
static int lockdown_set(int level)
{
    lock_down(level);
    return 0;
}

/* ── lockdown_release ────────────────────────────────────────── */
static int lockdown_release(void)
{
    /* Lockdown is irreversible; this is a no-op */
    kprintf("[LOCKDOWN] lockdown_release: lockdown is irreversible, ignored\n");
    return 0;
}

/* ── lockdown_restrict ───────────────────────────────────────── */
static int lockdown_restrict(const char *reason)
{
    kprintf("[LOCKDOWN] lockdown_restrict: %s\n", reason ? reason : "unspecified reason");
    lock_down(LOCKDOWN_INTEGRITY);
    return 0;
}

/* ── lockdown_get ─────────────────────────────── */
static int lockdown_get(void)
{
    return g_lockdown_level;
}

/* ── lockdown_is_locked ─────────────────────────────── */
static int lockdown_is_locked(int what)
{
    return lockdown_is_locked_down(what);
}
