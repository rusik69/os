/*
 * module.c — Kernel module loader stub
 *
 * Simple module registration table. Each module has a name, entry point,
 * and state. This is infrastructure for future loadable kernel modules.
 */

#include "module.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

static struct kernel_module g_modules[MODULE_MAX];
static spinlock_t g_mod_lock;
static int g_mod_initialized = 0;

void module_init(void) {
    memset(g_modules, 0, sizeof(g_modules));
    spinlock_init(&g_mod_lock);
    g_mod_initialized = 1;
    kprintf("[OK] Kernel module API initialized (%d slots)\n", MODULE_MAX);
}

int module_load(const char *name, module_entry_t entry) {
    if (!name || !entry || !g_mod_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    /* Check for duplicate name */
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state != MODULE_UNUSED &&
            strcmp(g_modules[i].name, name) == 0) {
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            return -1;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state == MODULE_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    strncpy(g_modules[slot].name, name, 31);
    g_modules[slot].name[31] = '\0';
    g_modules[slot].entry = entry;
    g_modules[slot].state = MODULE_LOADED;

    spinlock_irqsave_release(&g_mod_lock, irq_flags);

    kprintf("[MOD] Loaded module: %s (slot %d)\n", name, slot);
    return slot;
}

int module_unload(int module_id) {
    if (module_id < 0 || module_id >= MODULE_MAX || !g_mod_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    struct kernel_module *mod = &g_modules[module_id];
    if (mod->state == MODULE_UNUSED) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    kprintf("[MOD] Unloading module: %s (slot %d)\n", mod->name, module_id);

    mod->state = MODULE_UNUSED;
    mod->entry = NULL;
    memset(mod->name, 0, sizeof(mod->name));

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return 0;
}

struct kernel_module *module_find(const char *name) {
    if (!name || !g_mod_initialized) return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    struct kernel_module *found = NULL;
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state != MODULE_UNUSED &&
            strcmp(g_modules[i].name, name) == 0) {
            found = &g_modules[i];
            break;
        }
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return found;
}
