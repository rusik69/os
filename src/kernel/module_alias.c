/*
 * module_alias.c — Module alias matching engine (M38)
 *
 * Implements the alias table and glob-style pattern matching used by
 * the module autoloader to resolve device modalias strings (e.g.
 * "pci:v00008086d0000100Fsv00000000sd00000000bc06cc04") to module
 * names (e.g. "e1000") by comparing against registered alias patterns
 * declared via MODULE_ALIAS() in kernel modules.
 *
 * Lifecycle:
 *   1. During module loading (module_elf_finalize), aliases parsed from
 *      the .modinfo section are registered via module_alias_register().
 *   2. On module unload, aliases are cleaned up via module_alias_unregister().
 *   3. When request_module() is called with a modalias that doesn't
 *      directly match a module name, it falls back to module_alias_find().
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "module.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* ── Alias table ────────────────────────────────────────────────────
 *
 * Fixed-size table storing {pattern, module_name} pairs.  The table is
 * small (MODULE_ALIAS_MAX = 64 entries) since we only track one alias
 * per driver — many drivers will share the same PCI vendor/device
 * pattern, but the total number of distinct loadable modules is bounded.
 */

struct module_alias_entry {
    char pattern[MODULE_ALIAS_MAX_LEN];   /* glob pattern (e.g. "pci:v8086d100F*") */
    char owner[32];                        /* module that owns this alias */
    int  in_use;                           /* 1 = valid entry */
};

static struct module_alias_entry g_aliases[MODULE_ALIAS_MAX];
static spinlock_t g_alias_lock;

/* ── Glob-style pattern matching ────────────────────────────────────
 *
 * Matches a string against a glob pattern containing:
 *   *  — matches any sequence of characters (including empty)
 *   ?  — matches exactly one character
 *   All other characters match literally.
 *
 * This is a simplified glob implementation that does NOT support
 * character classes [...] or alternation, which are not needed for
 * standard PCI/USB modalias matching.
 */
static int glob_match(const char *pattern, const char *str)
{
    if (!pattern || !str)
        return 0;

    while (*pattern) {
        if (*pattern == '*') {
            /* Skip consecutive stars */
            while (*(pattern + 1) == '*')
                pattern++;
            pattern++;
            if (*pattern == '\0')
                return 1;  /* trailing * matches everything */

            /* Try matching remaining pattern at each position */
            while (*str) {
                if (glob_match(pattern, str))
                    return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            /* ? matches any single character */
            if (*str == '\0')
                return 0;
            pattern++;
            str++;
        } else {
            /* Literal character match (case-sensitive) */
            if (*pattern != *str)
                return 0;
            pattern++;
            str++;
        }
    }

    /* Both pointers at end → full match */
    return (*str == '\0');
}

/* ── Initialisation ───────────────────────────────────────────────── */

void module_alias_init(void)
{
    memset(g_aliases, 0, sizeof(g_aliases));
    spinlock_init(&g_alias_lock);
}

/* ── Registration / unregistration ────────────────────────────────── */

int module_alias_register(const char *pattern, const char *module_name)
{
    if (!pattern || !pattern[0] || !module_name || !module_name[0])
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_alias_lock, &irq_flags);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MODULE_ALIAS_MAX; i++) {
        if (!g_aliases[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_alias_lock, irq_flags);
        kprintf("[MOD_ALIAS] Warning: alias table full, dropping '%s' for '%s'\n",
                pattern, module_name);
        return -1;
    }

    strncpy(g_aliases[slot].pattern, pattern, MODULE_ALIAS_MAX_LEN - 1);
    g_aliases[slot].pattern[MODULE_ALIAS_MAX_LEN - 1] = '\0';
    strncpy(g_aliases[slot].owner, module_name, 31);
    g_aliases[slot].owner[31] = '\0';
    g_aliases[slot].in_use = 1;

    spinlock_irqsave_release(&g_alias_lock, irq_flags);

    kprintf("[MOD_ALIAS] Registered alias: '%s' → %s\n", pattern, module_name);
    return 0;
}

void module_alias_unregister(const char *module_name)
{
    if (!module_name || !module_name[0])
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_alias_lock, &irq_flags);

    int count = 0;
    for (int i = 0; i < MODULE_ALIAS_MAX; i++) {
        if (g_aliases[i].in_use &&
            strcmp(g_aliases[i].owner, module_name) == 0) {
            g_aliases[i].in_use = 0;
            count++;
        }
    }

    spinlock_irqsave_release(&g_alias_lock, irq_flags);

    if (count > 0) {
        kprintf("[MOD_ALIAS] Unregistered %d alias(es) for '%s'\n",
                count, module_name);
    }
}

/* ── Lookup ──────────────────────────────────────────────────────────
 *
 * module_alias_find — search the alias table for a matching module.
 *
 * @modalias:  The device identifier string to match (e.g. a PCI modalias
 *             generated by the PCI subsystem on device discovery).
 *
 * Returns a pointer to the module name (static storage valid until the
 * next call) or NULL if no match is found.
 *
 * This function iterates all registered alias patterns and tests each
 * using glob_match().  The first match wins.  Since the table is small
 * (MODULE_ALIAS_MAX = 64), linear scan is acceptable.
 */
const char *module_alias_find(const char *modalias)
{
    if (!modalias || !modalias[0])
        return NULL;

    /* Static buffer for the result — valid until next call.
     * This avoids heap allocation and is safe because the caller
     * (request_module) uses the result synchronously. */
    static char result[64];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_alias_lock, &irq_flags);

    for (int i = 0; i < MODULE_ALIAS_MAX; i++) {
        if (!g_aliases[i].in_use)
            continue;

        if (glob_match(g_aliases[i].pattern, modalias)) {
            strncpy(result, g_aliases[i].owner, sizeof(result) - 1);
            result[sizeof(result) - 1] = '\0';
            spinlock_irqsave_release(&g_alias_lock, irq_flags);
            return result;
        }
    }

    spinlock_irqsave_release(&g_alias_lock, irq_flags);
    return NULL;
}

/* ── Debug / diagnostics ──────────────────────────────────────────── */

/* Print all registered aliases to kprintf.  Used by the lsmod command
 * (via sys_query_module) and for debugging. */
void module_alias_dump(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_alias_lock, &irq_flags);

    int count = 0;
    for (int i = 0; i < MODULE_ALIAS_MAX; i++) {
        if (g_aliases[i].in_use) {
            kprintf("  [%2d] %-32s → %s\n",
                    i, g_aliases[i].pattern, g_aliases[i].owner);
            count++;
        }
    }

    spinlock_irqsave_release(&g_alias_lock, irq_flags);

    if (count == 0)
        kprintf("  (no aliases registered)\n");
    else
        kprintf("  Total: %d alias(es)\n", count);
}
