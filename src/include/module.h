#ifndef MODULE_H
#define MODULE_H

#include "types.h"
#include "list.h"
#include "vermagic.h"

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
    PARAM_TYPE_INT,      /* int */
    PARAM_TYPE_UINT,     /* unsigned int */
    PARAM_TYPE_CHARP,    /* char pointer (string, set to copy) */
    PARAM_TYPE_STRING,   /* fixed-size char buffer */
    PARAM_TYPE_BOOL,     /* bool (0/1, y/n, on/off) */
    PARAM_TYPE_INVALID,
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

/* Return the kernel's version magic (vermagic) string, used to verify
 * that a loadable module was built against a compatible kernel version
 * and with matching configuration flags. */
const char *module_get_vermagic(void);

/* Initialize the kernel module subsystem. */
void module_init(void);

/* Load a kernel module with the given name and entry function.
 * Returns module_id (>= 0) on success, or -1 on failure. */
int module_load(const char *name, module_entry_t entry);

/* Unload a previously loaded module by its module_id. */
int module_unload(int module_id);

/* Find a module by name. Returns NULL if not found. */
struct kernel_module *module_find(const char *name);

/* Return the name of the module at slot @id, or NULL if unused. */
const char *module_name_by_id(int id);

/* Return the module struct at slot @id, or NULL if unused/not live.
 * The caller must not hold the module lock across long operations. */
struct kernel_module *module_get_by_id(int id);

/* Return the number of currently loaded modules. */
int module_count(void);

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

/* Parse a param=val,param2=val2 string and set values on the module.
 * Called during module loading (sys_init_module / sys_finit_module).
 * Returns 0 on success, -EINVAL on parse error, -ENOENT for unknown param. */
int module_parse_params(struct kernel_module *mod, const char *params_str);

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

/* String module parameter — copies a string value into a fixed-size buffer.
 *
 * Usage: module_param_string(my_buf, buf, sizeof(buf), 0644);
 *
 * The parameter stores a NUL-terminated string in @buf (up to @size-1 bytes).
 * It uses PARAM_TYPE_STRING so that module_param_set_value() copies the
 * incoming string (truncating if necessary) and always NUL-terminates.
 */
#define module_param_string(name, buf, size, perm) \
    static struct kernel_param __module_param_str_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_STRING, \
        .data = (buf), \
        .data_len = (size), \
        .perm = (perm), \
        .set_fn = NULL, \
        .get_fn = NULL, \
    }; \
    __attribute__((constructor)) static void __register_param_str_##name(void) { \
        (void)__module_param_str_##name; \
    }

/* Array module parameter — parses comma-separated values into a C array.
 *
 * Usage: static int my_arr[16];
 *        static int my_arr_num;
 *        module_param_array(my_arr, int, &my_arr_num, 0644);
 *
 * @name:  parameter name (used as the sysfs entry name)
 * @type:  element type (int, uint, long, etc. — must match PARAM_TYPE_*)
 * @nump:  pointer to an int that receives the number of elements parsed
 * @perm:  sysfs permissions (0444, 0644, etc.)
 *
 * The macro declares a PARAM_TYPE_STRING parameter internally; the actual
 * parsing of comma-separated values is handled by a registration helper
 * that splits the string and converts each element.  For simplicity the
 * array is stored as a raw C array; the element count is written to @nump.
 *
 * NOTE: The array must be statically allocated with enough room; there is
 * no dynamic resizing.  The default max is 64 elements.
 */
#define module_param_array(name, type, nump, perm) \
    static int __module_param_array_##name##_parse(const char *val, \
                                                    struct kernel_param *kp) \
    { \
        (void)kp; \
        if (!val || !*val) return -1; \
        const char *p = val; \
        int count = 0; \
        while (*p && count < 64) { \
            /* Skip leading whitespace */ \
            while (*p == ' ' || *p == '\t') p++; \
            if (!*p) break; \
            long v = 0; \
            int neg = 0; \
            if (*p == '-') { neg = 1; p++; } \
            else if (*p == '+') p++; \
            while (*p >= '0' && *p <= '9') \
                v = v * 10 + (*p++ - '0'); \
            if (neg) v = -v; \
            ((type *)(name))[count++] = (type)v; \
            while (*p == ' ' || *p == '\t') p++; \
            if (*p == ',') p++; \
        } \
        if (nump) *(nump) = count; \
        return 0; \
    } \
    static struct kernel_param __module_param_arr_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_STRING, \
        .data = (name), \
        .data_len = sizeof(name), \
        .perm = (perm), \
        .set_fn = __module_param_array_##name##_parse, \
        .get_fn = NULL, \
    }; \
    __attribute__((constructor)) static void __register_param_arr_##name(void) { \
        (void)__module_param_arr_##name; \
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

/* Helper for token pasting with __LINE__ expansion */
#define __MODULE_PASTE(a, b)   a##b
#define _MODULE_PASTE(a, b)    __MODULE_PASTE(a, b)

/* MODULE_ALIAS — declare a device alias for module autoloading (M38).
 *
 * Usage in a kernel module source file:
 *   MODULE_ALIAS("pci:v00008086d0000100F*");
 *   MODULE_ALIAS("usb:v1234p5678*");
 *
 * This embeds "alias=<pattern>" in the .modinfo section.  When a device
 * is discovered (e.g. PCI enumeration), the autoloader generates a
 * modalias string and searches for a matching module by comparing it
 * (glob-style) against all registered alias patterns.
 *
 * Multiple aliases can be declared per module.  The pattern supports
 * glob-style wildcards:
 *   *  — matches any sequence of characters
 *   ?  — matches any single character
 */
#define MODULE_ALIAS(alias) \
    static const char _MODULE_PASTE(__mod_alias_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "alias=" alias

/* MODULE_* macros for metadata (used in .modinfo section) */
#define MODULE_LICENSE(license)    static const char _MODULE_PASTE(__mod_lic_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "license=" license
#define MODULE_AUTHOR(author)      static const char _MODULE_PASTE(__mod_auth_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "author=" author
#define MODULE_DESCRIPTION(desc)   static const char _MODULE_PASTE(__mod_desc_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "description=" desc
#define MODULE_VERSION(ver)        static const char _MODULE_PASTE(__mod_ver_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "version=" ver

/* MODULE_DEPENDS — declare module dependencies.
 *
 * Usage in a kernel module source file:
 *   MODULE_DEPENDS("ext2", "crc32");
 *
 * This embeds "depends=ext2,crc32" in the .modinfo section.  The module
 * loader ensures these modules are loaded (auto-loading if needed) before
 * this module's init function is called.
 *
 * Dependencies are separated by commas.  At most 16 dependencies are
 * supported (see MODULE_MAX_DEPS).
 */
#define MODULE_DEPENDS(deps) \
    static const char __mod_depends[] \
    __attribute__((section(".modinfo"), used)) = "depends=" deps

/* Set a module parameter from a string value (used by sysfs write callbacks).
 * Supports all standard parameter types. Returns 0 on success, -1 on error. */
int module_param_set_value(struct kernel_param *kp, const char *val);

/* Format a module parameter's current value as a string (used by sysfs read callbacks).
 * Returns the number of bytes written, or 0 on error. */
int module_param_format_value(struct kernel_param *kp, char *buf, int max);

/* ── Module autoloading (M34-M38) ─────────────────────────────────── */

/* Apply any cached boot-time cmdline parameters to a loaded module.
 * Called after the module's init function runs so that params
 * registered via module_add_param() are available. */
void module_apply_cmdline_params(struct kernel_module *mod);

/* Kernel-initiated module load — request a module by name.
 * Probes /modules/<name>.ko and loads it via the ELF loader.
 * May sleep; must not be called from atomic context.
 * Returns module ID (>0) on success, or a negative errno. */
int request_module(const char *fmt, ...);

/* request_module with explicit parameter string.
 * Like request_module(), but passes @params to the module's init. */
int request_module_params(const char *name, const char *params);

/* ── Module alias matching (M38) ──────────────────────────────────── */

/* Maximum number of module aliases the system can track */
#define MODULE_ALIAS_MAX 64

/* Maximum length of an alias pattern string */
#define MODULE_ALIAS_MAX_LEN 128

/* Initialise the module alias table.  Called from module_init(). */
void module_alias_init(void);

/* Register a module alias, mapping @pattern to @module_name.
 * Called during module loading for each alias parsed from .modinfo.
 * Returns 0 on success, -1 if the alias table is full. */
int module_alias_register(const char *pattern, const char *module_name);

/* Unregister all aliases owned by a module (by module name).
 * Called during module unloading. */
void module_alias_unregister(const char *module_name);

/* Find a module name that matches @modalias (device identifier string).
 * Performs glob-style pattern matching (*, ?) against all registered
 * alias patterns.  Returns the module name, or NULL if no match.
 * The returned pointer is valid until the next alias operation. */
const char *module_alias_find(const char *modalias);

/* ── Module sysfs interface (M30) ──────────────────────────────── */

/* Create sysfs entries for all parameters of a loaded module.
 * Creates /sys/module/<name>/parameters/<param> for each parameter.
 * Called after module_parse_params() succeeds during module loading. */
int module_sysfs_add_params(struct kernel_module *mod);

/* Remove all sysfs entries for a module's parameters.
 * Called before module_unload(). */
int module_sysfs_remove_params(struct kernel_module *mod);

#endif /* MODULE_H */
