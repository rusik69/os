#ifndef MODULE_H
#define MODULE_H

#include "types.h"
#include "list.h"

#define MODULE_MAX 16

/* Module states for the state machine */
enum module_state {
    MODULE_UNUSED    = 0,
    MODULE_LOADING   = 1,
    MODULE_LIVE      = 2,
    MODULE_UNLOADING = 3,
    MODULE_DEAD      = 4,
    MODULE_ERROR     = 5,
};

/* Module entry / exit point signatures */
typedef int (*module_entry_t)(void);
typedef void (*module_exit_t)(void);

/* Maximum number of dependencies a module can declare */
#define MODULE_MAX_DEPS 16

/* ── Module memory region ────────────────────────────────────────────
 * 64 MB virtual region in kernel space reserved for loadable modules.
 * Modules are mapped here with appropriate permissions:
 *   .text   → RX
 *   .rodata → RO
 *   .data   → RW
 *   .bss    → RW (zero-fill)
 */
#define MODULES_VADDR  0xFFFF800100000000ULL  /* 4 GB above kernel base */
#define MODULES_SIZE   0x04000000ULL           /* 64 MB                 */
#define MODULES_END    (MODULES_VADDR + MODULES_SIZE)

/* Module parameter types */
enum module_param_type {
    PARAM_TYPE_INT,
    PARAM_TYPE_CHAR,
    PARAM_TYPE_STRING,
};

/* A named module parameter */
struct kernel_param {
    char name[32];
    enum module_param_type type;
    void *data;           /* pointer to the actual variable */
    int data_len;
    int perm;             /* permissions (0444, 0644, etc.) */
    int (*set_fn)(const char *val, struct kernel_param *kp);
    int (*get_fn)(char *buf, int max, struct kernel_param *kp);
    struct list_head list;
};

/* Section descriptor — tracks a loaded ELF section within module memory */
struct module_section {
    uint64_t vaddr;       /* virtual address in module region */
    uint64_t size;        /* size in bytes */
    uint32_t sh_flags;    /* ELF section header flags (SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR) */
};

/* Module dependency entry */
struct module_dep {
    char name[32];        /* dependency module name */
    int  loaded;          /* 1 = already resolved and loaded */
};

/* Module registration entry — extended runtime descriptor */
struct kernel_module {
    char           name[32];
    module_entry_t entry;
    module_exit_t  exit_fn;
    enum module_state state;

    /* Memory region allocated for this module */
    uint64_t base_addr;        /* virtual base address in module region */
    uint64_t size;             /* total allocated size */

    /* Section tracking (up to 16 sections per module) */
    struct module_section sections[16];
    int num_sections;

    /* Lifecycle management */
    int    refcount;           /* reference counter (module_get / module_put) */
    int    module_id;          /* slot index in g_modules[] */

    /* Dependency tracking */
    struct module_dep deps[MODULE_MAX_DEPS];
    int   num_deps;

    /* Parameter list (linked list of kernel_param) */
    struct list_head params;
    int   param_count;
};

/* ── Kernel module API ────────────────────────────────────────────── */

/* Initialize the kernel module subsystem. */
void module_init(void);

/* Load a kernel module with the given name and entry function.
 * Returns module_id (>= 0) on success, or -1 on failure. */
int module_load(const char *name, module_entry_t entry);

/* Unload a previously loaded module by its module_id. */
int module_unload(int module_id);

/* Find a module by name. Returns NULL if not found. */
struct kernel_module *module_find(const char *name);

/* ── Module memory allocator (M10) ───────────────────────────────── */

/* Allocate a block of the given size from the module virtual region.
 * The region is mapped with the specified page flags (PAGE_RW, PAGE_NX, etc.)
 * Returns the virtual address, or 0 on failure. */
uint64_t module_alloc_region(uint64_t size, uint64_t page_flags);

/* Free a previously allocated module region. */
void module_free_region(uint64_t vaddr, uint64_t size);

/* Return the number of bytes currently allocated from the module region. */
uint64_t module_allocated_bytes(void);

/* ── Reference counting (M26 integration) ────────────────────────── */

/* Increment the reference count on a module. */
void module_get(struct kernel_module *mod);

/* Decrement the reference count on a module.
 * Returns 1 if the count reached 0, 0 otherwise. */
int module_put(struct kernel_module *mod);

/* ── Module parameter support ────────────────────────────────────── */

/* Register a named parameter for a module. */
int module_add_param(struct kernel_module *mod, const char *name,
                     enum module_param_type type, void *data, int data_len,
                     int perm, int (*set_fn)(const char*, struct kernel_param*),
                     int (*get_fn)(char*, int, struct kernel_param*));

/* Find a parameter in a module by name. */
struct kernel_param *module_find_param(struct kernel_module *mod, const char *name);

/* ── Dependency support (M23-M25 integration) ────────────────────── */

/* Add a dependency name to a module's dep list. */
int module_add_dep(struct kernel_module *mod, const char *dep_name);

/* Check whether all dependencies of a module are loaded. */
int module_deps_resolved(struct kernel_module *mod);

/* ── Convenience macros ──────────────────────────────────────────── */

/* Simple integer module parameter macro */
#define module_param(name, type, perm) \
    static struct kernel_param __module_param_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_##type, \
        .data = &name, \
        .data_len = sizeof(name), \
        .perm = perm, \
        .set_fn = NULL, \
        .get_fn = NULL, \
    }; \
    __attribute__((constructor)) static void __register_param_##name(void) { \
        (void)__module_param_##name; \
    }

/* Module parameter macro with callback functions */
#define module_param_cb(name, set_fn, get_fn) \
    static struct kernel_param __module_param_cb_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_INT, \
        .data = NULL, \
        .data_len = 0, \
        .perm = 0644, \
        .set_fn = set_fn, \
        .get_fn = get_fn, \
    }; \
    __attribute__((constructor)) static void __register_param_cb_##name(void) { \
        (void)__module_param_cb_##name; \
    }

/* MODULE_* macros for metadata (used in .modinfo section) */
#define MODULE_LICENSE(license)    static const char __mod_license[] \
    __attribute__((section(".modinfo"), used)) = "license=" license
#define MODULE_AUTHOR(author)      static const char __mod_author[] \
    __attribute__((section(".modinfo"), used)) = "author=" author
#define MODULE_DESCRIPTION(desc)   static const char __mod_desc[] \
    __attribute__((section(".modinfo"), used)) = "description=" desc
#define MODULE_VERSION(ver)        static const char __mod_version[] \
    __attribute__((section(".modinfo"), used)) = "version=" ver

#endif /* MODULE_H */
