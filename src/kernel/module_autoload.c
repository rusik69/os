/*
 * module_autoload.c — Kernel-initiated module loading (request_module)
 *
 * request_module() is the kernel's internal mechanism for autoloading
 * modules when they are needed.  It is called by subsystems when they
 * encounter a resource for which no handler is currently registered:
 *
 *   - PCI subsystem:   request_module("pci:v%08Xd%08Xsv%08X...")
 *   - VFS on mount:    request_module("ext2")
 *   - Socket create:   request_module("ipv6")
 *
 * The function searches the predefined module path (/modules/<name>.ko)
 * and loads the module using the same ELF loader as sys_init_module.
 *
 * request_module() can sleep (it may block on I/O and memory allocation)
 * and must not be called from atomic context.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "module.h"
#include "module_elf.h"
#include "printf.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "scheduler.h"

/* Variadic macros (freestanding environment — use compiler built-ins) */
#define REQ_MOD_VA_LIST    __builtin_va_list
#define REQ_MOD_VA_START   __builtin_va_start
#define REQ_MOD_VA_END     __builtin_va_end

/* Default module search path — relative to the root of the filesystem.
 * Modules are expected to live in /modules/<name>.ko, which mirrors the
 * standard Linux /lib/modules/ layout for an embedded/hobby OS.
 */
#define MODULE_PATH_PREFIX  "/modules/"
#define MODULE_PATH_SUFFIX  ".ko"
#define MODULE_PATH_MAX     128

/*
 * __request_module — load a kernel module by name.
 *
 * @name:   Module name (e.g. "ext2", "e1000", "ipv6").
 *          Used to construct the path /modules/<name>.ko.
 * @param:  Optional parameter string (can be NULL).
 *          Same format as insmod params: "key=val,key2=val2".
 * @flags:  Loading flags (reserved, must be 0 for now).
 *
 * Returns the module ID (> 0) on success, or a negative errno:
 *   -ENOENT    — module file not found
 *   -EEXIST    — module already loaded
 *   -ENOMEM    — out of memory
 *   -EINVAL    — invalid ELF or module
 *   -EIO       — I/O error reading module file
 *   -EPERM     — module loading disabled (e.g. module_sig_enforce)
 *
 * This function may sleep (it allocates memory and performs file I/O).
 * It must NOT be called from interrupt context or while holding a
 * spinlock (use a workqueue if you need asynchronous autoloading).
 */
int __request_module(const char *name, const char *params, int flags)
{
    (void)flags;  /* reserved for future use (e.g. synchronous vs async) */

    if (!name || !name[0])
        return -EINVAL;

    /* ── Step 0: Check if module is already loaded ───────────────── */
    if (module_find(name) != NULL) {
        kprintf("[MOD] request_module(%s): already loaded\n", name);
        return -EEXIST;
    }

    /* ── Step 1: Build path ──────────────────────────────────────── */
    char path[MODULE_PATH_MAX];
    int plen = snprintf(path, sizeof(path), "%s%s%s",
                        MODULE_PATH_PREFIX, name, MODULE_PATH_SUFFIX);
    if (plen < 0 || plen >= (int)sizeof(path))
        return -ENAMETOOLONG;

    /* ── Step 2: Stat the .ko file ───────────────────────────────── */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) {
        kprintf("[MOD] request_module(%s): file not found at %s\n",
                name, path);
        return -ENOENT;
    }

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 8 * 1024 * 1024) {
        kprintf("[MOD] request_module(%s): invalid file size %llu\n",
                name, (unsigned long long)file_size);
        return -EFBIG;
    }

    /* ── Step 3: Read the file into a kernel buffer ──────────────── */
    void *buf = kmalloc((size_t)file_size);
    if (!buf)
        return -ENOMEM;

    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    if (ret < 0 || bytes_read != file_size) {
        kprintf("[MOD] request_module(%s): read failed (ret=%d, read=%u/%llu)\n",
                name, ret, (unsigned)bytes_read, (unsigned long long)file_size);
        kfree(buf);
        return -EIO;
    }

    /* ── Step 4: Run the ELF module loader ───────────────────────── */
    struct module_elf_context ctx;
    int result = -1;

    /* Validate ELF header */
    if (module_elf_validate(&ctx, (const uint8_t *)buf, file_size) < 0) {
        kprintf("[MOD] request_module(%s): ELF validation failed: %s\n",
                name, ctx.error_msg);
        kfree(buf);
        return -EINVAL;
    }

    /* Parse ELF sections, symbols, relocations */
    if (module_elf_parse(&ctx) < 0) {
        kprintf("[MOD] request_module(%s): ELF parse failed: %s\n",
                name, ctx.error_msg);
        kfree(buf);
        return -EINVAL;
    }

    /* Finalize: resolve symbols, apply relocations, set perms, call init */
    result = module_elf_finalize(&ctx, name);
    module_elf_free(&ctx);
    kfree(buf);

    if (result < 0) {
        kprintf("[MOD] request_module(%s): finalize failed: %s\n",
                name, ctx.error_msg);
        return -EINVAL;
    }

    kprintf("[MOD] request_module(%s): loaded as id=%d\n", name, result);

    /* ── Step 5: Parse module parameters (if any) ────────────────── */
    if (params && params[0]) {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod) {
            int pret = module_parse_params(mod, params);
            if (pret < 0) {
                kprintf("[MOD] request_module(%s): parameter parsing failed (%d), "
                        "unloading\n", name, pret);
                module_unload(result);
                return (pret == -ENOENT) ? -ENOENT :
                       (pret == -ENOMEM) ? -ENOMEM : -EINVAL;
            }
            /* Create sysfs entries for parameters */
            module_sysfs_add_params(mod);
        }
    } else {
        /* Even without parameters, create sysfs entries */
        struct kernel_module *mod = module_get_by_id(result);
        if (mod)
            module_sysfs_add_params(mod);
    }

    return result;
}

/*
 * request_module — convenience wrapper around __request_module.
 *
 * This is the preferred interface for most callers.  It takes a format
 * string and arguments, builds the module name, and calls the loader.
 *
 * Examples:
 *   request_module("ext2");                         // simple name
 *   request_module("pci:v%08Xd%08X", vid, did);    // alias-based (future)
 */
int request_module(const char *fmt, ...)
{
    char name[64];
    REQ_MOD_VA_LIST args;
    REQ_MOD_VA_START(args, fmt);
    int nlen = vsnprintf(name, sizeof(name), fmt, args);
    REQ_MOD_VA_END(args);

    if (nlen < 0 || nlen >= (int)sizeof(name))
        return -ENAMETOOLONG;

    return __request_module(name, NULL, 0);
}

/*
 * request_module_params — load a module with parameters.
 *
 * Like request_module() but allows passing an initialiser string.
 * Useful for loading configuration-bearing modules from kernel code.
 */
int request_module_params(const char *name, const char *params)
{
    if (!name)
        return -EINVAL;

    /* If no params, just use the plain request_module path */
    if (!params || !params[0])
        return __request_module(name, NULL, 0);

    return __request_module(name, params, 0);
}
