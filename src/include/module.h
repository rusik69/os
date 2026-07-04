#ifndef MODULE_H
#define MODULE_H

#include "types.h"
#include "list.h"
#include "vermagic.h"
#include "sha256.h"

#define MODULE_MAX 16
#define MODULE_SIG_LEN 256

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

    /* Flags (MODULE_FLAG_ASYNC_PROBE, etc.) */
    uint32_t flags;

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
void modules_init(void);

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
        .perm = (perm), \
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
        if ((nump)) *(nump) = count; \
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

/*
 * MODULE_DEVICE_TABLE — declare a device ID table for module autoloading.
 *
 * This macro creates modalias patterns from a device ID table (struct
 * pci_device_id, struct usb_device_id, etc.) and embeds them into the
 * .modinfo section so the autoloader can match new devices to the module.
 *
 * Usage:
 *   static const struct pci_device_id my_pci_ids[] = {
 *       { PCI_DEVICE(0x8086, 0x100F), ... },
 *       { }
 *   };
 *   MODULE_DEVICE_TABLE(pci, my_pci_ids);
 *
 * The table must be terminated by an all-zero sentinel entry.
 * Supported bus types: pci, usb, virtio, i2c, platform.
 *
 * Under the hood this registers alias patterns equivalent to
 * MODULE_ALIAS("pci:v*d*sv*sd*bc*sc*i*") for each table entry.
 */
#define MODULE_DEVICE_TABLE(bus, table) \
    static const char _MODULE_PASTE(__mod_devtable_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "devtable=" #bus ":" #table

/* MODULE_* macros for metadata (used in .modinfo section) */
#define MODULE_LICENSE(license)    static const char _MODULE_PASTE(__mod_lic_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "license=" license
#define MODULE_AUTHOR(author)      static const char _MODULE_PASTE(__mod_auth_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "author=" author
#define MODULE_DESCRIPTION(desc)   static const char _MODULE_PASTE(__mod_desc_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "description=" desc
#define MODULE_VERSION(ver)        static const char _MODULE_PASTE(__mod_ver_, __LINE__)[] \
    __attribute__((section(".modinfo"), used)) = "version=" ver

/* ── module_init / module_exit — entry / exit point declarations ───────
 *
 * For built-in compilation (MODULE not defined):
 *   module_init(fn) registers fn as a device_initcall so it runs at boot.
 *   module_exit(fn) is a no-op (built-in code never unloads).
 *
 * For loadable module compilation (MODULE defined):
 *   module_init(fn) creates an 'init_module' alias so the ELF loader
 *   can find the entry point by name.
 *   module_exit(fn) creates a 'cleanup_module' alias for the exit path.
 *
 * Usage in any source file that can be built either way:
 *   static int my_init(void) { ... }
 *   static void my_exit(void) { ... }
 *   module_init(my_init);
 *   module_exit(my_exit);
 */
#ifdef MODULE
/* When building a loadable .ko, the module ELF loader looks for the
 * standard "init_module" and "cleanup_module" symbols.  We provide
 * wrapper functions that delegate to the user-defined init/exit.
 * We use wrappers instead of GCC __attribute__((alias)) because the
 * target function may be declared static. */
#define module_init(fn) \
    int init_module(void) { fn(); return 0; }
#define module_exit(fn) \
    void cleanup_module(void) { fn(); }
#else
/* Built-in: register via the initcall system so the function is called
 * automatically during kernel boot.  module_exit is a no-op since
 * built-in code cannot be unloaded at runtime. */
#include "initcall.h"
#define module_init(fn)    device_initcall(fn)
#define module_exit(fn)
#endif /* MODULE */

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
int request_module(const char *fmt, ...) __printf(1, 2);

/* request_module with explicit parameter string.
 * Like request_module(), but passes @params to the module's init. */
int request_module_params(const char *name, const char *params);

/* ── PCI/USB hotplug modalias generation (M38) ────────────────────── */

/*
 * Build a PCI modalias string from vendor/device/class info.
 * Returns the length written, or negative on error.
 */
int pci_modalias(uint16_t vid, uint16_t did,
                 uint16_t svid, uint16_t sdid,
                 uint8_t class, uint8_t subclass, uint8_t progif,
                 char *buf, int max_len);

/*
 * Build a USB modalias string from vendor/product/class info.
 * Returns the length written, or negative on error.
 */
int usb_modalias(uint16_t vid, uint16_t pid, uint16_t bcdDevice,
                 uint8_t bDeviceClass, uint8_t bDeviceSubClass,
                 uint8_t bDeviceProtocol,
                 char *buf, int max_len);

/*
 * Autoload a driver for a PCI device (hotplug path).
 * Generates modalias and calls request_module().
 */
int request_module_pci(uint16_t vid, uint16_t did,
                        uint16_t svid, uint16_t sdid,
                        uint8_t class, uint8_t subclass, uint8_t progif);

/*
 * Autoload a driver for a USB device (hotplug path).
 */
int request_module_usb(uint16_t vid, uint16_t pid, uint16_t bcdDevice,
                        uint8_t bDeviceClass, uint8_t bDeviceSubClass,
                        uint8_t bDeviceProtocol);

/* ── Module alias matching (M38) ──────────────────────────────────── */

/* Maximum number of module aliases the system can track */
#define MODULE_ALIAS_MAX 64

/* Maximum length of an alias pattern string */
#define MODULE_ALIAS_MAX_LEN 128

/* Initialise the module alias table.  Called from modules_init(). */
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

/* ── Module init ordering (D232 task 3) ──────────────────────────── */

/* Transition a module from MODULE_LOADING to MODULE_LIVE after its
 * init function succeeds.  Also triggers deferred init processing
 * (any modules waiting for this module will be re-checked).
 * Returns 0 on success, -EINVAL if mod is NULL or state is wrong. */
int module_set_live(struct kernel_module *mod);

/* Check whether all dependencies of @mod are in MODULE_LIVE state
 * (i.e., fully loaded and initialized).  Returns 1 if all deps are
 * live, 0 if any dependency is missing or still in LOADING state. */
int module_deps_all_live(struct kernel_module *mod);

/* Try to call a module's init function after ensuring all its
 * dependencies are in MODULE_LIVE state.
 *
 * If dependencies are not yet initialized, the module is queued
 * for deferred init and -EAGAIN is returned.  init will be retried
 * automatically when module_set_live() is called for a dependency.
 *
 * @mod:   The module to initialize (must be in MODULE_LOADING state)
 * @entry: The module's init entry function
 *
 * Returns 0 on success, negative errno on init failure, or -EAGAIN
 * if init was deferred (dependencies not yet ready). */
int module_init_with_deps(struct kernel_module *mod, module_entry_t entry);

/* Process all pending deferred inits.
 * Scans the deferred-init queue and calls module_init_with_deps()
 * for each entry whose dependencies are now all live.
 * Safe to call after any successful module_set_live(). */
void module_process_deferred_inits(void);

/* ── Module signing (appended signature support) ──────────────── */

/** Signature format appended to module binary (like Linux MODULE_SIG_STRING).
 *  The magic string "~Module signature appended~\n" is placed after the sig. */
#define MODULE_SIG_MAGIC      "~Module signature appended~\n"
#define MODULE_SIG_MAGIC_LEN  28  /* strlen of above */
#define MODULE_SIG_STRING_LEN (MODULE_SIG_MAGIC_LEN + MODULE_SIG_LEN + 4)

/* Module verification modes for /sys/kernel/module_verify */
#define MODULE_VERIFY_OFF      0   /* no verification */
#define MODULE_VERIFY_WARN     1   /* log warning, allow loading */
#define MODULE_VERIFY_ENFORCE  2   /* reject unsigned/invalid modules */

/** Extract an appended RSA+SHA256 signature from the end of module data.
 *  Scans for the magic string and extracts the 256-byte signature.
 *  @data      Pointer to module ELF data
 *  @data_len  Length of module data
 *  @sig_out   Output buffer for 256-byte RSA signature (or NULL to just check)
 *  @hash_out  Output buffer for SHA-256 hash (or NULL)
 *  Returns 0 on success (signature found), -ENOENT if no signature present. */
int modsig_extract(const uint8_t *data, size_t data_len,
                   uint8_t sig_out[MODULE_SIG_LEN],
                   uint8_t hash_out[SHA256_DIGEST_SIZE]);

/** Verify an appended RSA+SHA256 signature against the built-in public key.
 *  Computes SHA-256 of the module data (excluding signature), then verifies
 *  the RSA-2048 PKCS#1 v1.5 signature.
 *  @data      Pointer to module ELF data (WITH appended signature)
 *  @data_len  Length of module data including appended signature
 *  Returns 0 on success, -EKEYREJECTED on mismatch, -ENOENT if no signature. */
int modsig_verify(const uint8_t *data, size_t data_len);

/* Get the current module verification mode (0=off, 1=warn, 2=enforce). */
int module_verify_get_mode(void);

/* ── Module sysfs interface (M30) ──────────────────────────────── */

/* Create sysfs entries for all parameters of a loaded module.
 * Creates /sys/module/<name>/parameters/<param> for each parameter.
 * Called after module_parse_params() succeeds during module loading. */
int module_sysfs_add_params(struct kernel_module *mod);

/* Remove all sysfs entries for a module's parameters.
 * Called before module_unload(). */
int module_sysfs_remove_params(struct kernel_module *mod);

/* Build a /sys/module/<name> path string.  Exposed for testing. */
int build_mod_dir(char *buf, int max, const char *mod_name);

/* ── Legacy module syscall declarations (sys_module.c) ────────── */

/*
 * sys_create_module — Legacy module creation syscall (Linux 2.4 era).
 * Returns -ENOSYS; use finit_module/init_module instead.
 * Declared in module.h because module.h is included by syscall.c.
 */
uint64_t sys_create_module(uint64_t name_addr, uint64_t size);

/*
 * sys_get_kernel_syms — Legacy kernel symbol query syscall (Linux 2.4 era).
 * Returns -ENOSYS; use /proc/kallsyms instead.
 */
uint64_t sys_get_kernel_syms(uint64_t table_addr);

/*
 * sys_sysctl — Legacy sysctl(2) syscall (deprecated, Linux 2.0 era).
 * Returns -ENOSYS; use /proc/sys interface instead.
 */
uint64_t sys_sysctl(uint64_t args_addr);

#endif /* MODULE_H */
