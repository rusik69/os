#ifndef DYNAMIC_DEBUG_H
#define DYNAMIC_DEBUG_H

#include "types.h"

/*
 * Dynamic debug infrastructure.
 *
 * Allows enabling/disabling of pr_debug()-style messages at run-time
 * by function name.  Each call site is represented by a
 * dynamic_debug_descriptor which holds the function name and an
 * enabled/disabled flag.
 */

#define DYNAMIC_DEBUG_NAME_MAX  64

struct dynamic_debug_descriptor {
    const char *function;               /* function name */
    int         enabled;                /* 1 = enabled, 0 = disabled */
    uint32_t    line;                   /* source line (optional metadata) */
};

/*
 * dynamic_debug_enable  - Enable all descriptors matching 'func'.
 * If func is NULL, all descriptors are enabled.
 * Returns the number of descriptors matched.
 */
int dynamic_debug_enable(const char *func);

/*
 * dynamic_debug_disable  - Disable all descriptors matching 'func'.
 * If func is NULL, all descriptors are disabled.
 * Returns the number of descriptors matched.
 */
int dynamic_debug_disable(const char *func);

/*
 * dynamic_debug_register  - Register a descriptor for run-time control.
 * Called at each pr_debug() site via a macro (not exported to users).
 */
void dynamic_debug_register(struct dynamic_debug_descriptor *desc);

/*
 * dynamic_debug_enabled  - Check whether a given function's debug is on.
 * Returns non-zero if debug output should be emitted.
 */
int dynamic_debug_enabled(const char *func);

/*
 * dynamic_debug_init  - Global init.
 */
void dynamic_debug_init(void);

#endif /* DYNAMIC_DEBUG_H */
