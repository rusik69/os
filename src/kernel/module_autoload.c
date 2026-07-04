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
 *
 * MODULE_DEVICE_TABLE support:
 *   - PCI modalias: "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X"
 *   - USB modalias: "usb:v%04Xp%04Xd%04Xdc%02Xdsc%02X...")
 *   - Drivers declare MODULE_DEVICE_TABLE(pci, table) which embeds
 *     alias patterns in .modinfo
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

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Kernel-initiated module loading (request_module) — automatic module loading");
MODULE_AUTHOR("Ruslan Gustomiasov");

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
 * ── PCI modalias generation ──────────────────────────────────────────
 *
 * Generate a PCI modalias string from vendor/device/subvendor/subdevice
 * and class info.  Format (Linux-compatible):
 *
 *   pci:v0000VVVVd0000DDDDsv0000SSSSsd0000SSSSbc00CCsc00SSii00PP
 *
 * Wildcards (*) are used for fields that are zero (unspecified).
 */

#define MODALIAS_PCI_MAX    128
#define MODALIAS_USB_MAX    128

/*
 * Build a PCI modalias string.
 * @vid:      PCI vendor ID
 * @did:      PCI device ID
 * @svid:     PCI subsystem vendor ID (0 = any)
 * @sdid:     PCI subsystem device ID (0 = any)
 * @class:    PCI class code
 * @subclass: PCI subclass
 * @progif:   PCI programming interface
 * @buf:      Output buffer (must be >= MODALIAS_PCI_MAX)
 * Returns the length of the string written.
 */
int pci_modalias(uint16_t vid, uint16_t did,
                 uint16_t svid, uint16_t sdid,
                 uint8_t class, uint8_t subclass, uint8_t progif,
                 char *buf, int max_len)
{
    if (!buf || max_len < 32)
        return -EINVAL;

    return snprintf(buf, (size_t)max_len,
                    "pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X",
                    (unsigned int)vid,
                    (unsigned int)did,
                    (unsigned int)svid,
                    (unsigned int)sdid,
                    (unsigned int)class,
                    (unsigned int)subclass,
                    (unsigned int)progif);
}

/*
 * Build a USB modalias string.
 * @vid:      USB vendor ID
 * @pid:      USB product ID
 * @bcdDevice: Device release number
 * @bDeviceClass: Device class
 * @bDeviceSubClass: Device subclass
 * @bDeviceProtocol: Device protocol
 * @buf:      Output buffer (must be >= MODALIAS_USB_MAX)
 */
int usb_modalias(uint16_t vid, uint16_t pid, uint16_t bcdDevice,
                 uint8_t bDeviceClass, uint8_t bDeviceSubClass,
                 uint8_t bDeviceProtocol,
                 char *buf, int max_len)
{
    if (!buf || max_len < 32)
        return -EINVAL;

    return snprintf(buf, (size_t)max_len,
                    "usb:v%04Xp%04Xd%04Xdc%02Xdsc%02Xdp%02X",
                    (unsigned int)vid,
                    (unsigned int)pid,
                    (unsigned int)bcdDevice,
                    (unsigned int)bDeviceClass,
                    (unsigned int)bDeviceSubClass,
                    (unsigned int)bDeviceProtocol);
}

/*
 * ── Hotplug autoloading entry points ─────────────────────────────────
 *
 * These are called by the PCI/USB subsystem when a new device is
 * discovered and no driver is currently bound.  They generate the
 * appropriate modalias string and call request_module().
 */

/*
 * request_module_pci — Autoload a driver for a newly discovered PCI device.
 *
 * Called by the PCI enumeration code when a device has no driver yet.
 * Generates a modalias and calls request_module() to find and load a
 * matching module.
 */
int request_module_pci(uint16_t vid, uint16_t did,
                        uint16_t svid, uint16_t sdid,
                        uint8_t class, uint8_t subclass, uint8_t progif)
{
    char modalias[MODALIAS_PCI_MAX];
    int ret = pci_modalias(vid, did, svid, sdid,
                            class, subclass, progif,
                            modalias, sizeof(modalias));
    if (ret < 0)
        return ret;

    kprintf("[MOD_AUTOLOAD] PCI hotplug: attempting autoload for %s\\n",
            modalias);
    return request_module("%s", modalias);
}

/*
 * request_module_usb — Autoload a driver for a newly discovered USB device.
 *
 * Called by the USB enumeration code when a device has no driver yet.
 */
int request_module_usb(uint16_t vid, uint16_t pid, uint16_t bcdDevice,
                        uint8_t bDeviceClass, uint8_t bDeviceSubClass,
                        uint8_t bDeviceProtocol)
{
    char modalias[MODALIAS_USB_MAX];
    int ret = usb_modalias(vid, pid, bcdDevice,
                            bDeviceClass, bDeviceSubClass, bDeviceProtocol,
                            modalias, sizeof(modalias));
    if (ret < 0)
        return ret;

    kprintf("[MOD_AUTOLOAD] USB hotplug: attempting autoload for %s\\n",
            modalias);
    return request_module("%s", modalias);
}

/*
 * __request_module_internal — load a kernel module by name (no alias fallback).
 *
 * This is the core loader — try loading a module by its exact name.
 * Used directly for filesystem/protocol modules (ext2, ipv6, etc.)
 * and as the second step when alias resolution finds a match.
 *
 * See __request_module() for parameter/return documentation.
 */
static int __request_module_internal(const char *name, const char *params, int flags)
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
    if (plen < 0 || (size_t)plen >= sizeof(path))
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

    /* ── Step 5: Parse user-supplied module parameters (if any) ──
     * Boot-time cmdline parameters and sysfs entries were already
     * created by module_elf_finalize().  User-supplied params
     * (via request_module_params or the name=value in the alias
     * path) are applied here to override defaults. */
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
        }
    }

    return result;
}

/*
 * __request_module — load a kernel module by name or alias.
 *
 * First tries to load the module by @name directly (as a module name).
 * If that fails with -ENOENT (file not found), it treats @name as a
 * device modalias string and searches the alias table for a match.
 *
 * This secondary step is what makes PCI/USB/etc. autoloading work:
 * the PCI subsystem discovers a device and calls
 *   request_module("pci:v8086d100F...")
 * which fails as a module name but succeeds when the alias matcher
 * resolves "pci:v8086d100F*" → "e1000" and loads e1000.ko.
 *
 * See __request_module_internal() for parameter/return documentation.
 */
int __request_module(const char *name, const char *params, int flags)
{
    if (!name || !name[0])
        return -EINVAL;

    /* ── Attempt 1: Load by direct module name ───────────────────── */
    int result = __request_module_internal(name, params, flags);

    if (result >= 0 || result != -ENOENT) {
        /* Either loaded successfully, or failed for a reason other
         * than "file not found" — no alias fallback needed. */
        return result;
    }

    /* ── Attempt 2: Try alias-based resolution (M38) ────────────────
     *
     * The module file wasn't found by name.  Treat @name as a device
     * modalias string (e.g. "pci:v8086d100Fsv...bc...") and search
     * the alias table for a matching module.
     *
     * The PCI subsystem generates these modalias strings from device
     * config space; USB, ACPI, and other buses do similarly.  Driver
     * modules declare alias patterns via MODULE_ALIAS() in their source.
     */
    const char *mod_name = module_alias_find(name);
    if (!mod_name) {
        kprintf("[MOD] request_module(%s): no alias match found\n", name);
        return -ENOENT;
    }

    kprintf("[MOD] request_module(%s): alias resolved → %s\n",
            name, mod_name);

    /* Check if resolved module is already loaded */
    if (module_find(mod_name) != NULL) {
        kprintf("[MOD] request_module(%s): alias target %s already loaded\n",
                name, mod_name);
        return -EEXIST;
    }

    /* Load the resolved module by its real name */
    result = __request_module_internal(mod_name, params, flags);

    if (result < 0) {
        kprintf("[MOD] request_module(%s): alias target %s failed to load (%d)\n",
                name, mod_name, result);
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
__printf(1, 2)
int request_module(const char *fmt, ...)
{
    char name[64];
    REQ_MOD_VA_LIST args;
    REQ_MOD_VA_START(args, fmt);
    int nlen = vsnprintf(name, sizeof(name), fmt, args);
    REQ_MOD_VA_END(args);

    if (nlen < 0 || (size_t)nlen >= sizeof(name))
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

/* ── Stub: module_autoload ─────────────────────────────── */
int module_autoload(const char *name)
{
    (void)name;
    kprintf("[modload] module_autoload: not yet implemented\n");
    return 0;
}
/* ── Stub: module_autoload_alias ─────────────────────────────── */
int module_autoload_alias(const char *alias)
{
    (void)alias;
    kprintf("[modload] module_autoload_alias: not yet implemented\n");
    return 0;
}
