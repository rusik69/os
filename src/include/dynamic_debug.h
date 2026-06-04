#ifndef DYNAMIC_DEBUG_H
#define DYNAMIC_DEBUG_H

#include "types.h"

/*
 * Dynamic debug infrastructure.
 *
 * Allows enabling/disabling of pr_debug()-style messages at run-time
 * by function name, source file, or module name.  Each call site
 * registers a dynamic_debug_descriptor which holds metadata and an
 * enabled/disabled flag.
 *
 * Control is exercised through the debugfs file at:
 *   /sys/kernel/debug/dynamic_debug/control
 *
 * Command syntax:
 *   module <name> +p|-p   — enable/disable all sites in <module>
 *   file   <path>  +p|-p  — enable/disable all sites in <file>
 *   func   <name>  +p|-p  — enable/disable all sites in <function>
 *   all            +p|-p  — enable/disable all sites
 */

#define DYNAMIC_DEBUG_NAME_MAX  64

struct dynamic_debug_descriptor {
    const char *module;                 /* module name (NULL if built-in core) */
    const char *file;                   /* source file name */
    const char *function;              /* function name */
    uint32_t    line;                  /* source line (optional metadata) */
    int         enabled;               /* 1 = enabled, 0 = disabled */
};

/*
 * dynamic_debug_enable  - Enable all descriptors matching filter.
 * If filter is NULL, all descriptors are enabled.
 * match_type: 0=function, 1=file, 2=module
 * Returns the number of descriptors matched.
 */
int dynamic_debug_enable(const char *filter, int match_type);

/*
 * dynamic_debug_disable  - Disable all descriptors matching filter.
 * If filter is NULL, all descriptors are disabled.
 * match_type: 0=function, 1=file, 2=module
 * Returns the number of descriptors matched.
 */
int dynamic_debug_disable(const char *filter, int match_type);

/*
 * dynamic_debug_register  - Register a descriptor for run-time control.
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
