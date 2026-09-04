/*
 * lsm_stack.c — LSM module stacking layer.
 *
 * Maintains the ordered set of active Linux Security Modules.  Each
 * module joins the stack once during its init; the stack preserves the
 * registration (priority) order, which is the order in which modules are
 * consulted during a security decision.  Because each hook dispatcher in
 * lsm.c evaluates registered handlers sequentially and short-circuits on
 * the first non-zero return (first-fail), a module stacked earlier in the
 * registry takes precedence over those stacked later.
 *
 * This layer is deliberately independent of the per-hook handler lists:
 * a single module may contribute handlers to several hooks, and the stack
 * records only module membership and order.  The dispatchers already give
 * first-fail semantics across every handler of a hook regardless of which
 * module registered it.
 */

#include "lsm.h"
#include "printf.h"
#include "string.h"

/* Ordered registry of active modules. */
static const char *lsm_stack[LSM_STACK_MAX_MODULES];
static int lsm_stack_nr;
static int lsm_stack_initialized;

void lsm_stack_init(void) {
    int i;

    if (lsm_stack_initialized)
        return;

    for (i = 0; i < LSM_STACK_MAX_MODULES; i++)
        lsm_stack[i] = NULL;
    lsm_stack_nr = 0;
    lsm_stack_initialized = 1;
}

int lsm_stack_register(const char *name) {
    if (!name || !name[0])
        return -1;

    /* Refuse duplicates so the same module cannot join twice. */
    if (lsm_stack_has(name))
        return -1;

    if (lsm_stack_nr >= LSM_STACK_MAX_MODULES)
        return -1;

    lsm_stack[lsm_stack_nr] = name;
    lsm_stack_nr++;

    kprintf("[LSM] stacked %s (order=%d)\n", name, lsm_stack_nr - 1);
    return lsm_stack_nr - 1;
}

int lsm_stack_count(void) {
    return lsm_stack_nr;
}

const char *lsm_stack_name(int i) {
    if (i < 0 || i >= lsm_stack_nr)
        return NULL;
    return lsm_stack[i];
}

int lsm_stack_has(const char *name) {
    int i;

    if (!name)
        return 0;
    for (i = 0; i < lsm_stack_nr; i++) {
        if (strcmp(lsm_stack[i], name) == 0)
            return 1;
    }
    return 0;
}