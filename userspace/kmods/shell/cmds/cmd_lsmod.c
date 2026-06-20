/* cmd_lsmod.c — List loaded kernel modules (M22) */
#include "shell_cmds.h"
#include "printf.h"
#include "module.h"

void cmd_lsmod(void) {
    int n = module_count();
    if (n == 0) {
        kprintf("(no modules loaded)\n");
        return;
    }

    kprintf("%-24s %-8s %-8s\n", "Module", "ID", "Refs");
    kprintf("------------------------ ---------- --------\n");
    for (int i = 0; i < MODULE_MAX; i++) {
        const char *name = module_name_by_id(i);
        if (!name) continue;
        struct kernel_module *mod = module_get_by_id(i);
        if (!mod) continue;
        const char *state_str = "Live";
        switch (mod->state) {
            case MODULE_LOADING:   state_str = "Loading";  break;
            case MODULE_LIVE:      state_str = "Live";     break;
            case MODULE_UNLOADING: state_str = "Unloading"; break;
            case MODULE_DEAD:      state_str = "Dead";     break;
            case MODULE_ERROR:     state_str = "Error";    break;
            default:               state_str = "Unknown";  break;
        }
        kprintf("%-24s %-3d       %-2d [%s]\n",
                name, mod->module_id, mod->refcount, state_str);
    }
}
