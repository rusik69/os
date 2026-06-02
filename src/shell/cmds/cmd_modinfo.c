/* cmd_modinfo.c — modinfo: display kernel module information
 *
 * M28: modinfo shell command for the modular kernel transition.
 *
 * Usage:
 *   modinfo              — show all loaded modules with details
 *   modinfo <name>       — show details for a specific module
 *
 * Displays: name, state, refcount, base address, size,
 *           dependencies, and module parameters.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "module.h"

/* ── Module state to string ────────────────────────────────────────── */
static const char *mod_state_str(enum module_state state)
{
    switch (state) {
    case MODULE_UNUSED:    return "UNUSED";
    case MODULE_LOADING:   return "LOADING";
    case MODULE_LIVE:      return "LIVE";
    case MODULE_UNLOADING: return "UNLOADING";
    case MODULE_DEAD:      return "DEAD";
    case MODULE_ERROR:     return "ERROR";
    default:               return "?";
    }
}

/* ── Module parameter type to string ───────────────────────────────── */
static const char *param_type_str(enum module_param_type type)
{
    switch (type) {
    case PARAM_TYPE_INT:    return "int";
    case PARAM_TYPE_CHAR:   return "char";
    case PARAM_TYPE_STRING: return "string";
    default:                return "?";
    }
}

/* ── Display a single module's details ─────────────────────────────── */
static void show_module_info(struct kernel_module *mod)
{
    if (!mod) return;

    kprintf("Module:      %s\n", mod->name);
    kprintf("State:       %s\n", mod_state_str(mod->state));
    kprintf("Refcount:    %d\n", mod->refcount);
    kprintf("Base addr:   0x%llX\n", (unsigned long long)mod->base_addr);
    kprintf("Size:        %llu bytes (%llu KB)\n",
            (unsigned long long)mod->size,
            (unsigned long long)(mod->size / 1024));

    /* Dependencies */
    if (mod->num_deps > 0) {
        kprintf("Depends on:  ");
        for (int i = 0; i < mod->num_deps; i++) {
            if (i > 0) kprintf(", ");
            kprintf("%s%s", mod->deps[i].name,
                    mod->deps[i].loaded ? "" : " (unresolved)");
        }
        kprintf("\n");
    } else {
        kprintf("Depends on:  (none)\n");
    }

    /* Parameters */
    if (mod->param_count > 0) {
        kprintf("Parameters:  %d registered\n", mod->param_count);
        struct kernel_param *kp;
        int idx = 0;
        list_for_each_entry(kp, &mod->params, list) {
            kprintf("  [%d] %s (%s, perm 0%03o)\n",
                    idx++, kp->name, param_type_str(kp->type), kp->perm);
        }
    } else {
        kprintf("Parameters:  (none)\n");
    }

    kprintf("\n");
}

/* ── The modinfo command ───────────────────────────────────────────── */
void cmd_modinfo(const char *args)
{
    (void)args;

    int total = module_count();
    if (total == 0) {
        kprintf("No modules loaded.\n");
        return;
    }

    /* If an argument is given, show info for that specific module */
    if (args && *args) {
        /* Skip leading spaces */
        while (*args == ' ') args++;
        if (*args) {
            struct kernel_module *mod = module_find(args);
            if (!mod) {
                kprintf("modinfo: module '%s' not found\n", args);
                return;
            }
            show_module_info(mod);
            return;
        }
    }

    /* No argument: show brief info for all modules */
    kprintf("Module                State    Refcnt  Size     Deps  Params\n");
    kprintf("--------------------  -------  ------  -------  ----  ------\n");

    for (int i = 0; i < MODULE_MAX; i++) {
        const char *name = module_name_by_id(i);
        if (!name)
            continue;

        struct kernel_module *mod = module_find(name);
        if (!mod)
            continue;

        kprintf("%-20s  %-7s  %6d  %5lluK  %4d  %6d\n",
                mod->name,
                mod_state_str(mod->state),
                mod->refcount,
                (unsigned long long)(mod->size / 1024),
                mod->num_deps,
                mod->param_count);
    }

    kprintf("\nTotal: %d module(s) loaded.  Use 'modinfo <name>' for details.\n",
            total);
}
