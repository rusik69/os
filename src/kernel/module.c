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
#include "sysfs.h"
#include "vmm.h"
#include "pmm.h"
#include "errno.h"
#include "cmdline.h"
#include "aslr.h"

/* ── Boot-time module parameters (M33) ──────────────────────────────
 *
 * The kernel cmdline may include module parameters in the form:
 *
 *   module_name.param_name=value
 *
 * These are parsed at boot and cached.  When a module loads, any
 * matching parameters are applied automatically before the module's
 * entry function runs.  Examples:
 *
 *   e1000.debug=1
 *   ext2.verbose_mount=1
 *   my_mod.buf_size=4096
 *
 * The cache holds up to CMDLINE_MOD_PARAMS_MAX entries parsed from
 * the boot command line.  Each entry stores a module name, parameter
 * name, and value string.
 */
#define CMDLINE_MOD_PARAMS_MAX 32

struct cmdline_mod_param {
    char modname[32];   /* e.g. "e1000", "ext2" */
    char param[32];     /* e.g. "debug", "buf_size" */
    char value[128];    /* e.g. "1", "4096" */
    int  in_use;
};

/* Static pool for boot-time module parameters */
static struct cmdline_mod_param
    g_cmdline_params[CMDLINE_MOD_PARAMS_MAX];
static int g_cmdline_param_count = 0;

/* Forward declarations */
static void module_scan_cmdline_params(void);

/* ── Global module table ────────────────────────────────────────── */

/* ── Module version magic (vermagic) ────────────────────────────────
 *
 * This string records the kernel version and build configuration at the
 * time the kernel was compiled.  When a loadable module is inserted, the
 * module loader compares the module's embedded vermagic against this
 * string.  A mismatch causes the load to be rejected with a clear error
 * message, preventing subtle ABI corruption from incompatible modules.
 *
 * The string is defined by VERMAGIC_STRING from vermagic.h and
 * constructed from:
 *   - KVERSION  (kernel version, set in Makefile as -DKVERSION=...)
 *   - SMP flag  (CONFIG_SMP set in Makefile for SMP builds)
 *   - Preempt   (CONFIG_PREEMPT / CONFIG_PREEMPT_VOLUNTARY)
 *   - ARCH      (always "x86_64" for this port)
 */
const char module_vermagic[] = VERMAGIC_STRING;

/* Return the kernel's version magic string.  Used by the module loader
 * to compare against any module's embedded vermagic. */
const char *module_get_vermagic(void) {
    return module_vermagic;
}

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

/* KASLR offset (in bytes) for the module region base address.
 * Set once during module_init() to randomize where modules are loaded.
 * A non-zero offset shifts the usable region start, making module virtual
 * addresses unpredictable across boots. */
static uint64_t module_base_offset = 0;

void modules_init(void) {
    memset(g_modules, 0, sizeof(g_modules));
    for (int i = 0; i < MODULE_MAX; i++) {
        INIT_LIST_HEAD(&g_modules[i].params);
        g_modules[i].module_id = i;
    }
    spinlock_init(&g_mod_lock);
    module_region_allocated = 0;

    /* Compute a random base offset for KASLR of module addresses.
     * This shifts the start of the 64 MB module region by a random number
     * of pages (0..ASLR_MODULE_RANDOM_PAGES), making module virtual addresses
     * unpredictable across boots.  The shift is applied once at init. */
    uint64_t rand_pages = aslr_module_offset();
    module_base_offset = rand_pages * PAGE_SIZE;
    kprintf("[OK] Kernel module API initialized (%d slots, 64 MB region at 0x%lx + %lu MB KASLR offset)\n",
            MODULE_MAX, (unsigned long)MODULES_VADDR,
            (unsigned long)(module_base_offset >> 20));
    g_mod_initialized = 1;

    /* Initialise the module alias matching engine (M38) */
    module_alias_init();

    /* Log the kernel's vermagic string so boot logs can be cross-checked */
    kprintf("[OK] Kernel vermagic: %s\n", module_vermagic);

    /* Scan the kernel cmdline for module.param=value entries */
    module_scan_cmdline_params();
}

/* ── Module memory allocator (M10) ──────────────────────────────── */

uint64_t module_alloc_region(uint64_t size, uint64_t page_flags) {
    if (!g_mod_initialized || size == 0) return 0;

    /* Round size up to page boundary */
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    uint64_t next = MODULES_VADDR + module_base_offset + module_region_allocated;
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

    /* Check that vaddr falls within the randomized module region */
    uint64_t region_start = MODULES_VADDR + module_base_offset;
    if (vaddr < region_start || vaddr + size > MODULES_END) return;

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

/* ── Boot-time cmdline parameter helpers (M33) ──────────────────── */

/*
 * Scan the kernel command line for module parameters of the form
 *   module_name.param_name=value
 * and cache them for later application when each module loads.
 *
 * The cmdline raw string uses space-separated key=value pairs.
 * Keys that contain a '.' (dot) before the '=' are module parameters.
 * Example: "e1000.debug=1 ext2.verbose=1 root=/dev/sda1"
 *
 * Called once during module_init().
 */
static void module_scan_cmdline_params(void)
{
    memset(g_cmdline_params, 0, sizeof(g_cmdline_params));
    g_cmdline_param_count = 0;

    const char *raw = cmdline_raw();
    if (!raw || !raw[0])
        return;

    /* Parse the raw cmdline by tokenising on spaces.
     * Each token is either a standalone key or key=value. */
    char buf[1024];
    size_t raw_len = strlen(raw);
    if (raw_len >= sizeof(buf))
        raw_len = sizeof(buf) - 1;
    memcpy(buf, raw, raw_len);
    buf[raw_len] = '\0';

    const char *delim = " \t";
    char *token = strtok(buf, delim);
    while (token && g_cmdline_param_count < CMDLINE_MOD_PARAMS_MAX) {
        /* Look for a dot in the token before any '=' */
        const char *eq = strchr(token, '=');
        const char *dot = strchr(token, '.');

        if (dot && (!eq || dot < eq)) {
            /* This token looks like module.param=value */
            size_t mod_len = (size_t)(dot - token);
            const char *par_start = dot + 1;
            size_t par_len;
            const char *val_start = NULL;

            if (eq) {
                par_len = (size_t)(eq - par_start);
                val_start = eq + 1;
            } else {
                /* No '=' means param with no value (bool = 1) */
                par_len = strlen(par_start);
                /* val_start remains NULL */
            }

            if (mod_len > 0 && mod_len < 32 &&
                par_len > 0 && par_len < 32) {
                struct cmdline_mod_param *cp =
                    &g_cmdline_params[g_cmdline_param_count];

                memcpy(cp->modname, token, mod_len);
                cp->modname[mod_len] = '\0';
                memcpy(cp->param, par_start, par_len);
                cp->param[par_len] = '\0';

                if (val_start) {
                    size_t vlen = strlen(val_start);
                    if (vlen >= sizeof(cp->value))
                        vlen = sizeof(cp->value) - 1;
                    memcpy(cp->value, val_start, vlen);
                    cp->value[vlen] = '\0';
                } else {
                    cp->value[0] = '1';  /* bare module.param is like boolean true */
                    cp->value[1] = '\0';
                }

                cp->in_use = 1;
                g_cmdline_param_count++;

                kprintf("[MOD] Cmdline param: %s.%s=%s\n",
                        cp->modname, cp->param, cp->value);
            }
        }

        token = strtok(NULL, delim);
    }
}

/*
 * Apply any cached cmdline parameters that match a newly loaded module.
 * Called after a module's init function has run, so that any parameters
 * the module registered via module_add_param() are available.
 *
 * For each matching parameter, we construct "param=value" and call
 * module_parse_params() which handles individual param=value pairs.
 *
 * @mod: the newly loaded module (must be in MODULE_LIVE state)
 */
void module_apply_cmdline_params(struct kernel_module *mod)
{
    if (!mod || !mod->name[0])
        return;

    /* Build a parameter string for all matching cmdline entries.
     * We construct it as a comma-separated param=val list. */
    char params[512];
    int pos = 0;

    for (int i = 0; i < CMDLINE_MOD_PARAMS_MAX; i++) {
        if (!g_cmdline_params[i].in_use)
            continue;
        if (strcmp(g_cmdline_params[i].modname, mod->name) != 0)
            continue;

        /* Append "param=value" to the parameter string */
        int needed = (int)strlen(g_cmdline_params[i].param) + 1 +
                     (int)strlen(g_cmdline_params[i].value) + 2; /* "param=val," or "\0" */
        if (pos + needed > (int)sizeof(params))
            break;

        if (pos > 0)
            params[pos++] = ',';
        int n = snprintf(params + pos, sizeof(params) - (size_t)pos, "%s=%s",
                         g_cmdline_params[i].param,
                         g_cmdline_params[i].value);
        if (n > 0)
            pos += n;

        /* Mark as consumed so we don't apply it again */
        g_cmdline_params[i].in_use = 0;
    }

    if (pos == 0)
        return; /* no matching parameters */

    /* Apply the parameter string to the module */
    int ret = module_parse_params(mod, params);
    if (ret == 0) {
        kprintf("[MOD] Applied boot-time parameters to '%s': %s\n",
                mod->name, params);
    } else {
        kprintf("[MOD] Warning: boot-time parameters for '%s' "
                "partially applied (error %d): %s\n",
                mod->name, ret, params);
    }
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

    /* Unregister any aliases owned by this module (M38) */
    module_alias_unregister(mod->name);

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

/* Return the module struct at slot @id, or NULL if unused/not live. */
struct kernel_module *module_get_by_id(int id) {
    if (id < 0 || id >= MODULE_MAX || !g_mod_initialized) return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mod_lock, &irq_flags);

    struct kernel_module *mod = NULL;
    if (g_modules[id].state == MODULE_LIVE ||
        g_modules[id].state == MODULE_LOADING) {
        mod = &g_modules[id];
    }

    spinlock_irqsave_release(&g_mod_lock, irq_flags);
    return mod;
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

/* ── Module parameter parsing (M29) ─────────────────────────────── */

/*
 * module_parse_params — Parse a param=val,param2=val2 string.
 *
 * Called during module loading (from sys_init_module / sys_finit_module)
 * to apply the caller-supplied parameter string to the module's registered
 * kernel_param entries.
 *
 * The parameter string uses comma as the delimiter between key=value pairs.
 * Values containing commas are not supported (same as Linux's module_param).
 *
 * Supported types:
 *   PARAM_TYPE_INT    → strtol()
 *   PARAM_TYPE_UINT   → strtoul()
 *   PARAM_TYPE_BOOL   → 0/1, y/n, on/off, true/false
 *   PARAM_TYPE_CHARP  → kmalloc'd copy of value string
 *   PARAM_TYPE_STRING → strncpy into fixed-size buffer
 *
 * If the parameter has a custom set_fn, it is called instead of the
 * default type-based parsing.
 *
 * Returns 0 on success, -EINVAL on parse error, -ENOENT if param not found.
 */
int module_parse_params(struct kernel_module *mod, const char *params_str)
{
    if (!mod || !params_str || !params_str[0])
        return 0; /* No params to parse is not an error */

    /* Duplicate the string so we can safely strtok it */
    size_t len = strlen(params_str);
    if (len > 4096)
        return -EINVAL; /* sanity: prohibit >4K parameter blobs */

    char *buf = (char *)kmalloc(len + 1);
    if (!buf)
        return -ENOMEM;
    memcpy(buf, params_str, len + 1);

    int ret = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);

    while (token) {
        /* Skip leading whitespace */
        while (*token == ' ' || *token == '\t')
            token++;

        /* Skip empty tokens */
        if (*token == '\0') {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        /* Find the '=' separator */
        char *eq = strchr(token, '=');
        if (!eq) {
            /* Without '=', treat as boolean parameter set to 1 */
            /* The param name is the whole token */
            char *pname = token;
            /* Trim trailing whitespace from name */
            char *end = pname + strlen(pname) - 1;
            while (end > pname && (*end == ' ' || *end == '\t'))
                *end-- = '\0';

            if (*pname == '\0') {
                token = strtok_r(NULL, ",", &saveptr);
                continue;
            }

            struct kernel_param *kp = module_find_param(mod, pname);
            if (!kp) {
                kprintf("[MOD] Unknown parameter: '%s' for module '%s'\n",
                        pname, mod->name);
                ret = -ENOENT;
                break;
            }

            /* Boolean true / set to 1 for non-bool types */
            if (kp->set_fn) {
                if (kp->set_fn("1", kp) < 0) {
                    ret = -EINVAL;
                    break;
                }
            } else if (kp->data && kp->data_len >= (int)sizeof(int)) {
                *(int *)kp->data = 1;
            }
        } else {
            /* Split into name and value */
            *eq = '\0';
            char *pname = token;
            char *pval  = eq + 1;

            /* Trim trailing whitespace from name */
            char *end = pname + strlen(pname) - 1;
            while (end > pname && (*end == ' ' || *end == '\t'))
                *end-- = '\0';

            /* Trim leading whitespace from value */
            while (*pval == ' ' || *pval == '\t')
                pval++;

            if (*pname == '\0') {
                token = strtok_r(NULL, ",", &saveptr);
                continue;
            }

            struct kernel_param *kp = module_find_param(mod, pname);
            if (!kp) {
                kprintf("[MOD] Unknown parameter: '%s' for module '%s'\n",
                        pname, mod->name);
                ret = -ENOENT;
                break;
            }

            /* If the module registered a custom setter, use it */
            if (kp->set_fn) {
                if (kp->set_fn(pval, kp) < 0) {
                    kprintf("[MOD] Parameter '%s' set_fn rejected value '%s'\n",
                            pname, pval);
                    ret = -EINVAL;
                    break;
                }
                token = strtok_r(NULL, ",", &saveptr);
                continue;
            }

            /* Default type-based parsing */
            if (!kp->data || kp->data_len <= 0) {
                token = strtok_r(NULL, ",", &saveptr);
                continue;
            }

            switch (kp->type) {
            case PARAM_TYPE_INT: {
                if (kp->data_len < (int)sizeof(int)) break;
                char *endp = NULL;
                long val = strtol(pval, &endp, 0);
                if (endp == pval || (*endp != '\0' && !isspace((unsigned char)*endp))) {
                    ret = -EINVAL;
                    break;
                }
                *(int *)kp->data = (int)val;
                break;
            }
            case PARAM_TYPE_UINT: {
                if (kp->data_len < (int)sizeof(unsigned int)) break;
                char *endp = NULL;
                unsigned long val = strtoul(pval, &endp, 0);
                if (endp == pval || (*endp != '\0' && !isspace((unsigned char)*endp))) {
                    ret = -EINVAL;
                    break;
                }
                *(unsigned int *)kp->data = (unsigned int)val;
                break;
            }
            case PARAM_TYPE_BOOL: {
                if (kp->data_len < (int)sizeof(int)) break;
                unsigned long bval;
                /* Try numeric first */
                char *endp = NULL;
                bval = strtoul(pval, &endp, 0);
                if (endp != pval && (*endp == '\0' || isspace((unsigned char)*endp))) {
                    *(int *)kp->data = (bval != 0) ? 1 : 0;
                    break;
                }
                /* Try string keywords */
                if (strcmp(pval, "y") == 0 || strcmp(pval, "Y") == 0 ||
                    strcmp(pval, "yes") == 0 || strcmp(pval, "YES") == 0 ||
                    strcmp(pval, "on") == 0 || strcmp(pval, "ON") == 0 ||
                    strcmp(pval, "true") == 0 || strcmp(pval, "TRUE") == 0 ||
                    strcmp(pval, "1") == 0) {
                    *(int *)kp->data = 1;
                } else if (strcmp(pval, "n") == 0 || strcmp(pval, "N") == 0 ||
                           strcmp(pval, "no") == 0 || strcmp(pval, "NO") == 0 ||
                           strcmp(pval, "off") == 0 || strcmp(pval, "OFF") == 0 ||
                           strcmp(pval, "false") == 0 || strcmp(pval, "FALSE") == 0 ||
                           strcmp(pval, "0") == 0) {
                    *(int *)kp->data = 0;
                } else {
                    ret = -EINVAL;
                }
                break;
            }
            case PARAM_TYPE_CHARP: {
                /* Free previous allocation if any */
                char *old = *(char **)kp->data;
                if (old) kfree(old);

                size_t vlen = strlen(pval);
                char *copy = (char *)kmalloc(vlen + 1);
                if (!copy) {
                    ret = -ENOMEM;
                    break;
                }
                memcpy(copy, pval, vlen + 1);
                *(char **)kp->data = copy;
                kp->data_len = (int)(vlen + 1);
                break;
            }
            case PARAM_TYPE_STRING: {
                /* Fixed-size buffer: copy up to data_len-1 chars */
                size_t copy_len = strlen(pval);
                if (copy_len >= (size_t)kp->data_len)
                    copy_len = (size_t)kp->data_len - 1;
                memset(kp->data, 0, (size_t)kp->data_len);
                memcpy(kp->data, pval, copy_len);
                break;
            }
            default:
                ret = -EINVAL;
                break;
            }

            if (ret < 0) {
                kprintf("[MOD] Parameter '%s': failed to parse value '%s' (type=%d)\n",
                        pname, pval, (int)kp->type);
                break;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    kfree(buf);
    return ret;
}

/* ── Module parameter sysfs interface (M30) ────────────────────── */

/* Buffer size for formatting a single parameter value */
#define MOD_PARAM_BUF_SIZE 128

/*
 * Format a kernel parameter's current value into a string buffer.
 * Returns the number of bytes written (excluding NUL), or 0 on error.
 * Exported for use by sysfs read callbacks.
 */
int module_param_format_value(struct kernel_param *kp, char *buf, int max)
{
    if (!kp || !buf || max <= 0)
        return 0;

    /* If the module registered a custom getter, use it */
    if (kp->get_fn)
        return kp->get_fn(buf, max, kp);

    if (!kp->data || kp->data_len <= 0)
        return 0;

    switch (kp->type) {
    case PARAM_TYPE_INT:
        if (kp->data_len >= (int)sizeof(int))
            return snprintf(buf, (size_t)max, "%d", *(int *)kp->data);
        break;
    case PARAM_TYPE_UINT:
        if (kp->data_len >= (int)sizeof(unsigned int))
            return snprintf(buf, (size_t)max, "%u", *(unsigned int *)kp->data);
        break;
    case PARAM_TYPE_BOOL:
        if (kp->data_len >= (int)sizeof(int))
            return snprintf(buf, (size_t)max, "%s", *(int *)kp->data ? "Y" : "N");
        break;
    case PARAM_TYPE_CHARP:
        if (kp->data_len >= (int)sizeof(char *) && *(char **)kp->data)
            return snprintf(buf, (size_t)max, "%s", *(char **)kp->data);
        return snprintf(buf, (size_t)max, "(null)");
    case PARAM_TYPE_STRING:
        if (kp->data && kp->data_len > 0)
            return snprintf(buf, (size_t)max, "%s", (const char *)kp->data);
        break;
    default:
        break;
    }
    return 0;
}

/*
 * Set a kernel parameter's value from a string.
 * Compatible with the sysfs_write_cb_t signature.
 */
static int module_param_write_cb(const char *data, uint32_t size, void *priv)
{
    struct kernel_param *kp = (struct kernel_param *)priv;
    if (!kp || !data || size == 0)
        return -1;

    /* Allocate a NUL-terminated copy of the written data */
    uint32_t copy_len = size;
    if (copy_len > MOD_PARAM_BUF_SIZE - 1)
        copy_len = MOD_PARAM_BUF_SIZE - 1;

    char val_buf[MOD_PARAM_BUF_SIZE];
    memcpy(val_buf, data, copy_len);
    val_buf[copy_len] = '\0';

    /* Strip trailing newline/carriage return if present */
    while (copy_len > 0 && (val_buf[copy_len - 1] == '\n' || val_buf[copy_len - 1] == '\r'))
        val_buf[--copy_len] = '\0';

    return module_param_set_value(kp, val_buf);
}

/*
 * Set a module parameter from a parsed string value.
 * This is the core implementation used by both module_parse_params and
 * the sysfs write callback.
 * Exported for use by sysfs read/write wrappers.
 */
int module_param_set_value(struct kernel_param *kp, const char *val)
{
    if (!kp || !val)
        return -1;

    /* If the module registered a custom setter, delegate to it */
    if (kp->set_fn)
        return kp->set_fn(val, kp);

    if (!kp->data || kp->data_len <= 0)
        return -1;

    switch (kp->type) {
    case PARAM_TYPE_INT: {
        if (kp->data_len < (int)sizeof(int)) return -1;
        char *endp = NULL;
        long v = strtol(val, &endp, 0);
        if (endp == val || (*endp != '\0' && !isspace((unsigned char)*endp)))
            return -1;
        *(int *)kp->data = (int)v;
        return 0;
    }
    case PARAM_TYPE_UINT: {
        if (kp->data_len < (int)sizeof(unsigned int)) return -1;
        char *endp = NULL;
        unsigned long v = strtoul(val, &endp, 0);
        if (endp == val || (*endp != '\0' && !isspace((unsigned char)*endp)))
            return -1;
        *(unsigned int *)kp->data = (unsigned int)v;
        return 0;
    }
    case PARAM_TYPE_BOOL: {
        if (kp->data_len < (int)sizeof(int)) return -1;
        /* Try numeric first */
        char *endp = NULL;
        unsigned long bval = strtoul(val, &endp, 0);
        if (endp != val && (*endp == '\0' || isspace((unsigned char)*endp))) {
            *(int *)kp->data = (bval != 0) ? 1 : 0;
            return 0;
        }
        /* String keywords */
        if (strcmp(val, "y") == 0 || strcmp(val, "Y") == 0 ||
            strcmp(val, "yes") == 0 || strcmp(val, "YES") == 0 ||
            strcmp(val, "on") == 0 || strcmp(val, "ON") == 0 ||
            strcmp(val, "true") == 0 || strcmp(val, "TRUE") == 0 ||
            strcmp(val, "1") == 0) {
            *(int *)kp->data = 1;
            return 0;
        }
        if (strcmp(val, "n") == 0 || strcmp(val, "N") == 0 ||
            strcmp(val, "no") == 0 || strcmp(val, "NO") == 0 ||
            strcmp(val, "off") == 0 || strcmp(val, "OFF") == 0 ||
            strcmp(val, "false") == 0 || strcmp(val, "FALSE") == 0 ||
            strcmp(val, "0") == 0) {
            *(int *)kp->data = 0;
            return 0;
        }
        return -1;
    }
    case PARAM_TYPE_CHARP: {
        char *old = *(char **)kp->data;
        if (old) kfree(old);
        size_t vlen = strlen(val);
        char *copy = (char *)kmalloc(vlen + 1);
        if (!copy) return -1;
        memcpy(copy, val, vlen + 1);
        *(char **)kp->data = copy;
        kp->data_len = (int)(vlen + 1);
        return 0;
    }
    case PARAM_TYPE_STRING: {
        size_t copy_len = strlen(val);
        if (copy_len >= (size_t)kp->data_len)
            copy_len = (size_t)kp->data_len - 1;
        memset(kp->data, 0, (size_t)kp->data_len);
        memcpy(kp->data, val, copy_len);
        return 0;
    }
    default:
        return -1;
    }
    return 0;
}

/*
 * Sysfs read callback for a module parameter.
 * Compatible with sysfs_read_cb_t signature.
 */
static int module_param_read_cb(char *buf, uint32_t max_size, void *priv)
{
    struct kernel_param *kp = (struct kernel_param *)priv;
    if (!kp || !buf || max_size == 0)
        return 0;
    return module_param_format_value(kp, buf, (int)max_size);
}

/* ── Module sysfs path construction helpers ────────────────────── */

/* Build /sys/module/<name>/parameters/ path for a module.
 * Returns the length written, or 0 on truncation. */
static int build_mod_param_dir(char *buf, int max, const char *mod_name)
{
    return snprintf(buf, (size_t)max, "/sys/module/%s/parameters", mod_name);
}

/* Build /sys/module/<name>/parameters/<param> path.
 * Returns the length written, or 0 on truncation. */
static int build_mod_param_path(char *buf, int max, const char *mod_name,
                                 const char *param_name)
{
    return snprintf(buf, (size_t)max, "/sys/module/%s/parameters/%s",
                    mod_name, param_name);
}

/* Build /sys/module/<name> path.
 * Returns the length written, or 0 on truncation. */
static int build_mod_dir(char *buf, int max, const char *mod_name)
{
    return snprintf(buf, (size_t)max, "/sys/module/%s", mod_name);
}

/*
 * Create sysfs entries for all parameters of a loaded module.
 * This creates:
 *   /sys/module/<name>/
 *   /sys/module/<name>/parameters/
 *   /sys/module/<name>/parameters/<param>  (one per parameter)
 *
 * Called after module_parse_params() succeeds during module loading.
 */
int module_sysfs_add_params(struct kernel_module *mod)
{
    if (!mod || mod->name[0] == '\0')
        return -1;

    /* Lazily create /sys/module/ directory if it doesn't exist yet */
    static int sysfs_module_dir_created = 0;
    if (!sysfs_module_dir_created) {
        if (sysfs_create_dir("/sys/module") < 0) {
            /* sysfs might not be mounted yet — defer silently */
            return -1;
        }
        sysfs_module_dir_created = 1;
    }

    /* Check if the module has any parameters */
    if (list_empty(&mod->params))
        return 0; /* No params — nothing to do */

    /* Create /sys/module/<name>/ directory */
    char mod_dir[128];
    if (build_mod_dir(mod_dir, (int)sizeof(mod_dir), mod->name) <= 0)
        return -1;
    sysfs_create_dir(mod_dir);

    /* Create /sys/module/<name>/parameters/ directory */
    char param_dir[128];
    if (build_mod_param_dir(param_dir, (int)sizeof(param_dir), mod->name) <= 0)
        return -1;
    sysfs_create_dir(param_dir);

    /* Create one file per parameter */
    struct kernel_param *kp;
    list_for_each_entry(kp, &mod->params, list) {
        if (kp->name[0] == '\0')
            continue;

        char param_path[160];
        if (build_mod_param_path(param_path, (int)sizeof(param_path),
                                  mod->name, kp->name) <= 0)
            continue;

        /* Create a writable sysfs file with read/write callbacks.
         * The priv (void*) is the kernel_param pointer. */
        sysfs_create_writable_file(param_path, NULL, (void *)kp,
            module_param_read_cb, module_param_write_cb);
    }

    return 0;
}

/*
 * Remove all sysfs entries for a module's parameters.
 * Called before module_unload().
 */
int module_sysfs_remove_params(struct kernel_module *mod)
{
    if (!mod || mod->name[0] == '\0')
        return -1;

    char mod_dir[128];
    if (build_mod_dir(mod_dir, (int)sizeof(mod_dir), mod->name) <= 0)
        return -1;

    /* Recursively remove the entire module directory in sysfs */
    sysfs_remove_recursive(mod_dir);
    return 0;
}
