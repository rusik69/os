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
#include "heap.h"

static struct kernel_module g_modules[MODULE_MAX];
static spinlock_t g_mod_lock;
static int g_mod_initialized = 0;

void module_init(void) {
    memset(g_modules, 0, sizeof(g_modules));
    for (int i = 0; i < MODULE_MAX; i++) {
        INIT_LIST_HEAD(&g_modules[i].params);
    }
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
    INIT_LIST_HEAD(&g_modules[slot].params);
    g_modules[slot].param_count = 0;

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

/* ── Module parameter support ──────────────────────────────────── */

int module_add_param(struct kernel_module *mod, const char *name,
                     enum module_param_type type, void *data, int data_len,
                     int perm, int (*set_fn)(const char*, struct kernel_param*),
                     int (*get_fn)(char*, int, struct kernel_param*)) {
    if (!mod || !name) return -1;

    struct kernel_param *kp = (struct kernel_param *)kmalloc(sizeof(struct kernel_param));
    if (!kp) return -1;

    memset(kp, 0, sizeof(*kp));
    strncpy(kp->name, name, 31);
    kp->name[31] = '\0';
    kp->type = type;
    kp->data = data;
    kp->data_len = data_len;
    kp->perm = perm;
    kp->set_fn = set_fn;
    kp->get_fn = get_fn;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);
    list_add_tail(&kp->list, &mod->params);
    mod->param_count++;
    spinlock_irqsave_release(&g_mod_lock, irq_flags);

    return 0;
}

struct kernel_param *module_find_param(struct kernel_module *mod, const char *name) {
    if (!mod || !name) return NULL;

    struct kernel_param *kp;
    list_for_each_entry(kp, &mod->params, list) {
        if (strcmp(kp->name, name) == 0) return kp;
    }
    return NULL;
}
