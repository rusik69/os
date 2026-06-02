/*
 * module.c — Kernel module loader with ELF-load infrastructure
 *
 * Implements the module state machine (UNLOADED→LOADING→LIVE→UNLOADING→DEAD),
 * a 64MB module memory region allocator with per-section permissions,
 * reference counting, and dependency tracking.
 *
 * M9: Extended struct kernel_module with base address, sections, refcount.
 * M10: Module memory region allocator (MODULES_VADDR .. MODULES_VADDR+64MB).
 */

#include "module.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "heap.h"
#include "vmm.h"
#include "pmm.h"

/* ── Global module table ────────────────────────────────────────── */

static struct kernel_module g_modules[MODULE_MAX];
static spinlock_t g_mod_lock;
static int g_mod_initialized = 0;

/* ── Module memory region state (M10) ──────────────────────────────
 *
 * We manage a simple bump allocator within the 64 MB virtual region
 * reserved for loadable modules.  Each allocation is page-aligned and
 * mapped with the requested page flags (RX, RW, RO, etc.).
 *
 * The region is initially unmapped; pages are committed on demand.
 */

/* Current allocation cursor (virtual address within the region) */
static uint64_t module_region_allocated = 0;  /* bytes consumed so far */

void module_init(void) {
    memset(g_modules, 0, sizeof(g_modules));
    for (int i = 0; i < MODULE_MAX; i++) {
        INIT_LIST_HEAD(&g_modules[i].params);
        g_modules[i].module_id = i;
    }
    spinlock_init(&g_mod_lock);
    module_region_allocated = 0;
    g_mod_initialized = 1;
    kprintf("[OK] Kernel module API initialized (%d slots, 64 MB region at 0x%llX)\n",
            MODULE_MAX, (unsigned long long)MODULES_VADDR);
}

/* ── Module memory allocator (M10) ──────────────────────────────── */

uint64_t module_alloc_region(uint64_t size, uint64_t page_flags) {
    if (!g_mod_initialized || size == 0) return 0;

    /* Round size up to page boundary */
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    uint64_t next = MODULES_VADDR + module_region_allocated;
    if (next + size > MODULES_END) {
        /* Out of module virtual space */
        kprintf("[MOD] module_alloc_region: out of memory (requested %llu, used %llu / %llu)\n",
                (unsigned long long)size,
                (unsigned long long)module_region_allocated,
                (unsigned long long)MODULES_SIZE);
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return 0;
    }

    /* Map pages with the requested flags */
    /* We use the kernel page table (get_kernel_pml4 / kernel_pml4) — currently
     * accessed via vmm_map_page with KERNEL_VMA_OFFSET identity semantics.
     *
     * For now, allocate physical pages and map them into the module region.
     * A proper implementation would use lazy / on-demand mapping.
     */
    uint64_t vaddr = next;
    uint64_t pages = size / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            /* Allocation failure — unwind already-mapped pages */
            for (uint64_t j = 0; j < i; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                uint64_t pa = vmm_get_physaddr(va);
                if (pa) {
                    vmm_unmap_page(va);
                    pmm_free_frame(pa);
                }
            }
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            kprintf("[MOD] module_alloc_region: pmm_alloc_frame failed\n");
            return 0;
        }

        if (vmm_map_page(vaddr + i * PAGE_SIZE, phys, page_flags) < 0) {
            /* Unmap on failure */
            pmm_free_frame(phys);
            for (uint64_t j = 0; j < i; j++) {
                uint64_t va = vaddr + j * PAGE_SIZE;
                uint64_t pa = vmm_get_physaddr(va);
                if (pa) {
                    vmm_unmap_page(va);
                    pmm_free_frame(pa);
                }
            }
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            kprintf("[MOD] module_alloc_region: vmm_map_page failed\n");
            return 0;
        }
    }

    module_region_allocated += size;
    spinlock_irqsave_release(&g_mod_lock, irq_flags);

    return vaddr;
}

void module_free_region(uint64_t vaddr, uint64_t size) {
    if (!g_mod_initialized || vaddr == 0 || size == 0) return;
    if (vaddr < MODULES_VADDR || vaddr + size > MODULES_END) return;

    /* Round size up to page boundary */
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    uint64_t pages = size / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = vaddr + i * PAGE_SIZE;
        uint64_t pa = vmm_get_physaddr(va);
        if (pa) {
            vmm_unmap_page(va);
            pmm_free_frame(pa);
        }
    }

    /* For a bump allocator we don't truly reclaim; in a full implementation
     * we would track free regions.  The module region is large enough (64 MB)
     * that exhaustion is unlikely during normal operation. */
    spinlock_irqsave_release(&g_mod_lock, irq_flags);
}

uint64_t module_allocated_bytes(void) {
    return module_region_allocated;
}

/* ── Module loading ─────────────────────────────────────────────── */

int module_load(const char *name, module_entry_t entry) {
    if (!name || !entry || !g_mod_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    /* Check for duplicate name */
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state != MODULE_UNUSED &&
            g_modules[i].state != MODULE_DEAD &&
            strcmp(g_modules[i].name, name) == 0) {
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            return -1;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state == MODULE_UNUSED ||
            g_modules[i].state == MODULE_DEAD) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    struct kernel_module *mod = &g_modules[slot];
    memset(mod, 0, sizeof(*mod));
    strncpy(mod->name, name, 31);
    mod->name[31] = '\0';
    mod->entry = entry;
    mod->exit_fn = NULL;
    mod->state = MODULE_LOADING;
    mod->base_addr = 0;
    mod->size = 0;
    mod->num_sections = 0;
    mod->refcount = 0;
    mod->num_deps = 0;
    mod->param_count = 0;
    mod->module_id = slot;
    INIT_LIST_HEAD(&mod->params);

    /* Transition to LIVE — the caller is expected to call the entry point.
     * If the entry succeeds, the caller should call module_set_live(mod). */
    mod->state = MODULE_LIVE;

    spinlock_irqsave_release(&g_mod_lock, irq_flags);

    kprintf("[MOD] Loaded module: %s (slot %d)\n", name, slot);
    return slot;
}

int module_unload(int module_id) {
    if (module_id < 0 || module_id >= MODULE_MAX || !g_mod_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    struct kernel_module *mod = &g_modules[module_id];
    if (mod->state == MODULE_UNUSED || mod->state == MODULE_DEAD) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    /* Check refcount — if still in use, refuse to unload */
    if (mod->refcount > 0) {
        kprintf("[MOD] Cannot unload %s: refcount=%d\n", mod->name, mod->refcount);
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    kprintf("[MOD] Unloading module: %s (slot %d)\n", mod->name, module_id);

    /* Call module exit function if one was registered */
    mod->state = MODULE_UNLOADING;
    if (mod->exit_fn) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        mod->exit_fn();
        spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);
    }

    /* Free module memory region */
    if (mod->base_addr != 0 && mod->size > 0) {
        module_free_region(mod->base_addr, mod->size);
    }

    /* Clear the slot */
    memset(mod, 0, sizeof(*mod));
    INIT_LIST_HEAD(&mod->params);
    mod->state = MODULE_DEAD;
    mod->module_id = module_id;

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return 0;
}

struct kernel_module *module_find(const char *name) {
    if (!name || !g_mod_initialized) return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    struct kernel_module *found = NULL;
    for (int i = 0; i < MODULE_MAX; i++) {
        if ((g_modules[i].state == MODULE_LIVE ||
             g_modules[i].state == MODULE_LOADING) &&
            strcmp(g_modules[i].name, name) == 0) {
            found = &g_modules[i];
            break;
        }
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return found;
}

/* ── Module enumeration (for sys_query_module) ────────────────────── */

/* Return the name of the module at slot @id, or NULL if that slot is unused. */
const char *module_name_by_id(int id) {
    if (id < 0 || id >= MODULE_MAX || !g_mod_initialized) return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    const char *name = NULL;
    if (g_modules[id].state == MODULE_LIVE ||
        g_modules[id].state == MODULE_LOADING) {
        name = g_modules[id].name;
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return name;
}

/* Return the number of currently loaded modules. */
int module_count(void) {
    if (!g_mod_initialized) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    int count = 0;
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state == MODULE_LIVE ||
            g_modules[i].state == MODULE_LOADING) {
            count++;
        }
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return count;
}

/* ── Reference counting (M26) ───────────────────────────────────── */

void module_get(struct kernel_module *mod) {
    if (!mod) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);
    if (mod->state == MODULE_LIVE) {
        mod->refcount++;
    }
    spinlock_irqsave_release(&g_mod_lock, irq_flags);
}

int module_put(struct kernel_module *mod) {
    if (!mod) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);
    if (mod->refcount > 0) {
        mod->refcount--;
    }
    int reached_zero = (mod->refcount == 0);
    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return reached_zero;
}

/* ── Module parameter support ───────────────────────────────────── */

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

/* ── Dependency support (M23-M25) ───────────────────────────────── */

int module_add_dep(struct kernel_module *mod, const char *dep_name) {
    if (!mod || !dep_name) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    if (mod->num_deps >= MODULE_MAX_DEPS) {
        spinlock_irqsave_release(&g_mod_lock, irq_flags);
        return -1;
    }

    /* Check for duplicate */
    for (int i = 0; i < mod->num_deps; i++) {
        if (strcmp(mod->deps[i].name, dep_name) == 0) {
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            return 0; /* already present, not an error */
        }
    }

    strncpy(mod->deps[mod->num_deps].name, dep_name, 31);
    mod->deps[mod->num_deps].name[31] = '\0';
    mod->deps[mod->num_deps].loaded = 0;
    mod->num_deps++;

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return 0;
}

int module_deps_resolved(struct kernel_module *mod) {
    if (!mod) return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    for (int i = 0; i < mod->num_deps; i++) {
        /* Check if the dependency is loaded and live */
        struct kernel_module *dep = NULL;
        for (int j = 0; j < MODULE_MAX; j++) {
            if ((g_modules[j].state == MODULE_LIVE) &&
                strcmp(g_modules[j].name, mod->deps[i].name) == 0) {
                dep = &g_modules[j];
                break;
            }
        }
        if (!dep) {
            spinlock_irqsave_release(&g_mod_lock, irq_flags);
            return 0; /* dependency not loaded */
        }
        mod->deps[i].loaded = 1;
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return 1; /* all deps resolved */
}
