/* cmd_rmmod.c — Unload a kernel module by name (M22) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "module.h"

void cmd_rmmod(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: rmmod <module_name>\n");
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        kprintf("Usage: rmmod <module_name>\n");
        return;
    }

    /* Extract module name (first word only) */
    char name[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(name) - 1) {
        name[i++] = *p++;
    }
    name[i] = '\0';

    /* Find the module by name */
    struct kernel_module *mod = module_find(name);
    if (!mod) {
        kprintf("rmmod: module '%s' not found\n", name);
        return;
    }

    int mod_id = mod->module_id;
    const char *mod_name = mod->name;

    /* Remove sysfs parameter entries before unloading */
    module_sysfs_remove_params(mod);

    /* Unload the module */
    int ret = module_unload(mod_id);
    if (ret < 0) {
        kprintf("rmmod: failed to unload '%s' (refcount may be non-zero)\n", mod_name);
        return;
    }

    kprintf("rmmod: unloaded '%s'\n", mod_name);
}
