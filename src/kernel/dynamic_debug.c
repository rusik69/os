#include "dynamic_debug.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "heap.h"
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
static int dyndbg_count;
static spinlock_t dyndbg_lock;

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

void dynamic_debug_init(void)
{
    spinlock_init(&dyndbg_lock);
    dyndbg_count = 0;
    kprintf("[OK] dynamic_debug: Dynamic debug control initialised\n");
}
