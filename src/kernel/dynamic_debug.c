#include "dynamic_debug.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "heap.h"
#include "spinlock.h"

/*
 * Dynamic debug: simple table-based lookup by function name.
 *
 * Each call site registers a descriptor; enable/disable walks the
 * table and flips the enabled flag for matching entries.
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

int dynamic_debug_enable(const char *func)
{
    int matched = 0;

    spinlock_acquire(&dyndbg_lock);
    for (int i = 0; i < dyndbg_count; i++) {
        if (!func || strcmp(dyndbg_table[i]->function, func) == 0) {
            dyndbg_table[i]->enabled = 1;
            matched++;
        }
    }
    spinlock_release(&dyndbg_lock);
    return matched;
}

int dynamic_debug_disable(const char *func)
{
    int matched = 0;

    spinlock_acquire(&dyndbg_lock);
    for (int i = 0; i < dyndbg_count; i++) {
        if (!func || strcmp(dyndbg_table[i]->function, func) == 0) {
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
