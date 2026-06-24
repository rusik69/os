#include "dynamic_debug.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "spinlock.h"
#include "debugfs.h"

/*
 * Dynamic debug: table-based lookup by function, file, or module.
 *
 * Each call site registers a descriptor via dynamic_debug_register().
 * The debugfs control file at /sys/kernel/debug/dynamic_debug/control
 * allows runtime toggling with commands like:
 *   module ahci +p     — enable pr_debug in ahci module
 *   file   ahci.c -p   — disable pr_debug in ahci.c
 *   func   probe +p    — enable pr_debug in probe() function
 *   all -p             — disable all pr_debug
 *
 * The dyndbg.c driver (drivers/dyndbg.c) provides the debugfs file
 * and command-line parsing; this file provides the core matching
 * engine and descriptor registry.
 */

#define DYNDBG_MAX_DESCS  256

static struct dynamic_debug_descriptor *dyndbg_table[DYNDBG_MAX_DESCS];
static int dyndbg_count = 0;
static spinlock_t dyndbg_lock;

/**
 * dynamic_debug_register - Register a dynamic debug descriptor
 * @desc: Descriptor containing function, file, module name and enabled flag
 *
 * Adds a call-site descriptor to the global dynamic debug table.
 * Registered descriptors can be matched and toggled at runtime via
 * the debugfs control interface or by dynamic_debug_enable/disable().
 * The table holds up to DYNDBG_MAX_DESCS entries.
 */
void dynamic_debug_register(struct dynamic_debug_descriptor *desc)
{
    if (!desc)
        return;

    spinlock_acquire(&dyndbg_lock);
    if (dyndbg_count < DYNDBG_MAX_DESCS)
        dyndbg_table[dyndbg_count++] = desc;
    spinlock_release(&dyndbg_lock);
}

/*
 * Match a descriptor against a filter.
 *
 * @desc:  the descriptor to check
 * @filter: the string to match (NULL matches everything)
 * @match_type: 0=function name, 1=file name, 2=module name
 *
 * Returns 1 if the descriptor matches, 0 otherwise.
 */
static int dyndbg_matches(struct dynamic_debug_descriptor *desc,
                           const char *filter, int match_type)
{
    const char *field;

    switch (match_type) {
    case 2: /* module */
        field = desc->module;
        break;
    case 1: /* file */
        field = desc->file;
        break;
    case 0: /* function */
    default:
        field = desc->function;
        break;
    }

    if (!filter)
        return 1;  /* NULL filter matches all */
    if (!field)
        return 0;  /* descriptor has no such field */
    return (strcmp(field, filter) == 0);
}

/**
 * dynamic_debug_enable - Enable pr_debug for descriptors matching a filter
 * @filter: String to match (NULL matches all descriptors)
 * @match_type: Match field selector (0=function, 1=file, 2=module)
 *
 * Iterates all registered descriptors and sets enabled=1 on those
 * whose function/file/module field matches @filter.  Returns the
 * number of descriptors that were matched.
 *
 * Return: Number of descriptors matched and enabled
 */
int dynamic_debug_enable(const char *filter, int match_type)
{
    int matched = 0;

    spinlock_acquire(&dyndbg_lock);
    for (int i = 0; i < dyndbg_count; i++) {
        if (dyndbg_matches(dyndbg_table[i], filter, match_type)) {
            dyndbg_table[i]->enabled = 1;
            matched++;
        }
    }
    spinlock_release(&dyndbg_lock);
    return matched;
}

/**
 * dynamic_debug_disable - Disable pr_debug for descriptors matching a filter
 * @filter: String to match (NULL matches all descriptors)
 * @match_type: Match field selector (0=function, 1=file, 2=module)
 *
 * Iterates all registered descriptors and sets enabled=0 on those
 * whose function/file/module field matches @filter.  Returns the
 * number of descriptors that were matched.
 *
 * Return: Number of descriptors matched and disabled
 */
int dynamic_debug_disable(const char *filter, int match_type)
{
    int matched = 0;

    spinlock_acquire(&dyndbg_lock);
    for (int i = 0; i < dyndbg_count; i++) {
        if (dyndbg_matches(dyndbg_table[i], filter, match_type)) {
            dyndbg_table[i]->enabled = 0;
            matched++;
        }
    }
    spinlock_release(&dyndbg_lock);
    return matched;
}

/**
 * dynamic_debug_enabled - Check whether pr_debug is enabled for a function
 * @func: Function name to look up
 *
 * Searches the registered descriptors for one matching @func and
 * returns its enabled state.  This is the fast-path query used by
 * pr_debug() / pr_devel() macros at runtime.
 *
 * Return: 1 if pr_debug is enabled for @func, 0 otherwise
 */
int dynamic_debug_enabled(const char *func)
{
    if (!func)
        return 0;

    spinlock_acquire(&dyndbg_lock);
    for (int i = 0; i < dyndbg_count; i++) {
        if (strcmp(dyndbg_table[i]->function, func) == 0) {
            int en = dyndbg_table[i]->enabled;
            spinlock_release(&dyndbg_lock);
            return en;
        }
    }
    spinlock_release(&dyndbg_lock);
    return 0;
}

/**
 * dynamic_debug_init - Initialise the dynamic debug subsystem
 *
 * Initialises the spinlock and resets the descriptor table.
 * Called once during kernel boot.  Must be invoked before any
 * dynamic_debug_register() or query operations.
 */
void __init dynamic_debug_init(void)
{
    spinlock_init(&dyndbg_lock);
    dyndbg_count = 0;
    kprintf("[OK] dynamic_debug: Dynamic debug control initialised\n");
}

/* ── Stub: dynamic_debug_query ─────────────────────────────── */
/**
 * dynamic_debug_query - Parse and execute a dynamic debug control command
 * @query: Command string (e.g., \"module ahci +p\", \"func probe -p\")
 *
 * Processes a single dynamic debug control command.  The command
 * syntax follows the standard \"func|file|module <name> +p|-p\" format.
 * Currently a stub — full implementation is in the dyndbg.c driver.
 *
 * Return: 0 on success, negative on parse error
 */
int dynamic_debug_query(const char *query)
{
    (void)query;
    kprintf("[dyndbg] dynamic_debug_query: not yet implemented\n");
    return 0;
}
