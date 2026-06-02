/* cmd_rmmod.c — rmmod: unload a kernel module by name
 *
 * M22: rmmod shell command for the modular kernel transition.
 *
 * Usage: rmmod <name>
 *
 * Calls module_find + module_unload to remove a loaded module.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "module.h"

void cmd_rmmod(const char *args)
{
    if (!args || !args[0]) {
        kprintf("Usage: rmmod <name>\n");
        return;
    }

    /* Extract the module name (first token) */
    char name[64];
    const char *p = args;
    char *d = name;
    while (*p && *p != ' ' && (size_t)(d - name) < sizeof(name) - 1) {
        *d++ = *p++;
    }
    *d = '\0';

    if (name[0] == '\0') {
        kprintf("rmmod: missing module name\n");
        return;
    }

    /* Find the module */
    struct kernel_module *mod = module_find(name);
    if (!mod) {
        kprintf("rmmod: module '%s' not found\n", name);
        return;
    }

    /* Check refcount */
    if (mod->refcount > 0) {
        kprintf("rmmod: '%s' is in use (refcount=%d)\n", name, mod->refcount);
        return;
    }

    /* Unload the module */
    int result = module_unload(mod->module_id);
    if (result < 0) {
        kprintf("rmmod: failed to unload '%s'\n", name);
    } else {
        kprintf("rmmod: Unloaded '%s'\n", name);
    }
}
