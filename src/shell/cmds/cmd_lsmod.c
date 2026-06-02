/* cmd_lsmod.c — lsmod: list loaded kernel modules
 *
 * M22: lsmod shell command for the modular kernel transition.
 *
 * Usage: lsmod
 *
 * Iterates the module table using module_name_by_id() and prints
 * each loaded module's name, state, reference count, and size.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "module.h"

void cmd_lsmod(const char *args)
{
    (void)args;

    int count = module_count();
    if (count == 0) {
        kprintf("No modules loaded.\n");
        return;
    }

    kprintf("Module                State  Refcnt  Size\n");
    kprintf("--------------------  -----  ------  ------\n");

    for (int i = 0; i < MODULE_MAX; i++) {
        const char *name = module_name_by_id(i);
        if (!name)
            continue;

        /* We need to get the state and refcount for printing.
         * Since module_name_by_id only returns the name, we need
         * to use module_find to get the full struct. */
        struct kernel_module *mod = module_find(name);
        if (!mod)
            continue;

        /* Format: name left-padded to 20 chars, then state, refcount, size */
        const char *state_str;
        switch (mod->state) {
        case MODULE_LOADING:   state_str = "LOAD";  break;
        case MODULE_LIVE:      state_str = "LIVE";  break;
        case MODULE_UNLOADING: state_str = "UNLD";  break;
        case MODULE_DEAD:      state_str = "DEAD";  break;
        case MODULE_ERROR:     state_str = "ERR";   break;
        default:               state_str = "?";     break;
        }

        kprintf("%-20s  %-5s  %5d  %llu\n",
                name, state_str, mod->refcount,
                (unsigned long long)mod->size);
    }
}
