/* cmd_modprobe.c — Dependency-aware kernel module loader (M27)
 *
 * Usage: modprobe <name> [param=val ...]
 *
 * Locates /modules/<name>.ko, loads it and all its dependencies
 * automatically.  Unlike insmod, takes a module name (not path)
 * and uses the kernel's built-in dependency resolution.
 *
 * The kernel's module loader handles:
 *   - Parsing .modinfo for dependency declarations
 *   - Topological sort and cycle detection
 *   - Auto-loading missing dependencies via request_module()
 *   - Parameter parsing (module_param etc.)
 */
#include "shell_cmds.h"
#include "module.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Where module .ko files live */
#define MODULE_PATH       "/modules/"
#define MODULE_PATH_LEN   9

/* ── Help message ─────────────────────────────────────────────────── */
static void print_usage(void)
{
    kprintf("Usage: modprobe <name> [param=val ...]\n"
            "  <name>          Module name (without .ko suffix)\n"
            "  param=val       Optional module parameters\n"
            "\n"
            "Examples:\n"
            "  modprobe ext2\n"
            "  modprobe e1000 debug=1\n"
            "  modprobe ipv6\n");
}

/* ── The modprobe command ──────────────────────────────────────────── */
void cmd_modprobe(const char *args)
{
    if (!args || !*args) {
        print_usage();
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        print_usage();
        return;
    }

    /* Extract module name (first token) */
    char name[64];
    int ni = 0;
    while (*args && *args != ' ' && ni < (int)sizeof(name) - 1)
        name[ni++] = *args++;
    name[ni] = '\0';

    if (ni == 0) {
        kprintf("modprobe: empty module name\n");
        return;
    }

    /* Validate module name: only alphanumeric, underscores, hyphens */
    for (int i = 0; name[i]; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '_' || name[i] == '-')) {
            kprintf("modprobe: invalid character '%c' in module name '%s'\n",
                    name[i], name);
            return;
        }
    }

    /* Skip whitespace to find parameters */
    while (*args == ' ') args++;
    const char *params = (*args) ? args : NULL;

    /* Check if module is already loaded */
    struct kernel_module *existing = module_find(name);
    if (existing && existing->state == MODULE_LIVE) {
        kprintf("modprobe: module '%s' is already loaded (refcount=%d)\n",
                name, existing->refcount);
        return;
    }

    /* Build the full path: /modules/<name>.ko */
    char path[128];
    int pi = 0;

    /* Copy prefix */
    const char *prefix = MODULE_PATH;
    while (*prefix && pi < (int)sizeof(path) - 1)
        path[pi++] = *prefix++;
    /* Copy name */
    for (int i = 0; name[i] && pi < (int)sizeof(path) - 5; i++)
        path[pi++] = name[i];
    /* Append .ko suffix */
    path[pi++] = '.';
    path[pi++] = 'k';
    path[pi++] = 'o';
    path[pi] = '\0';

    kprintf("modprobe: loading '%s'...\n", name);

    /* Use request_module_params() to load the module.
     * This kernel function handles:
     *   1. Checking if already loaded
     *   2. Reading /modules/<name>.ko
     *   3. ELF validation, parsing
     *   4. Dependency resolution (topological sort, cycle detection)
     *   5. Symbol resolution, relocation
     *   6. Module init() call
     *   7. Parameter application
     */
    int ret = request_module_params(name, params);

    if (ret >= 0) {
        /* Success — show what was loaded */
        struct kernel_module *mod = module_find(name);
        if (mod) {
            kprintf("modprobe: '%s' loaded successfully (id=%d, size=%llu bytes)\n",
                    name, ret, (unsigned long long)mod->size);

            /* Show dependencies that were loaded along with it */
            if (mod->num_deps > 0) {
                kprintf("  Dependencies:");
                for (int d = 0; d < mod->num_deps; d++) {
                    kprintf(" %s%s", mod->deps[d].name,
                            mod->deps[d].loaded ? "" : " (missing)");
                }
                kprintf("\n");
            }

            /* Show parameter count */
            if (mod->param_count > 0) {
                kprintf("  Parameters: %d registered\n", mod->param_count);
            }
        } else {
            kprintf("modprobe: '%s' loaded successfully (id=%d)\n",
                    name, ret);
        }
    } else {
        /* Failure — print diagnostic information */
        int err = -ret;
        const char *err_msg = "Unknown error";

        switch (err) {
        case 1:  err_msg = "Operation not permitted"; break;         /* EPERM  */
        case 2:  err_msg = "No such file or directory"; break;       /* ENOENT */
        case 5:  err_msg = "I/O error"; break;                       /* EIO    */
        case 12: err_msg = "Out of memory"; break;                   /* ENOMEM */
        case 17: err_msg = "File exists"; break;                     /* EEXIST */
        case 22: err_msg = "Invalid ELF or module file"; break;      /* EINVAL */
        case 27: err_msg = "File too large (>8MB)"; break;           /* EFBIG  */
        case 63: err_msg = "Bad executable (ELF format)"; break;     /* ENOEXEC*/
        case 93: err_msg = "Dependency not available"; break;        /* ENOPROTOOPT */
        default:
            /* Build a more specific message for common module errors */
            if (err == 93) err_msg = "Dependency resolution failed";
            break;
        }

        kprintf("modprobe: failed to load '%s': %s (errno=%d)\n",
                name, err_msg, err);

        /* Suggest checking the path */
        kprintf("  Looked for: %s\n", path);

        /* If the file doesn't exist, suggest module name or location */
        if (err == 2) {
            /* Check if there's a similarly named module loaded already */
            int found_similar = 0;
            for (int i = 0; i < MODULE_MAX; i++) {
                const char *mod_name = module_name_by_id(i);
                if (mod_name && strncmp(mod_name, name, strlen(name)) == 0) {
                    if (!found_similar) {
                        kprintf("  Did you mean one of these loaded modules?\n");
                        found_similar = 1;
                    }
                    kprintf("    %s\n", mod_name);
                }
            }
            if (!found_similar) {
                kprintf("  Ensure the .ko file exists in %s\n", MODULE_PATH);
                kprintf("  Usage: modprobe <name>  (name without .ko suffix)\n");
            }
        }
    }
}
