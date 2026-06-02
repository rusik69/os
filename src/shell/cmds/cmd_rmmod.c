/* cmd_rmmod.c — Unload a kernel module (M22)
 *
 * Usage: rmmod <name>
 *
 * Calls the SYS_DELETE_MODULE syscall to unload a module by name.
 */
#include "shell_cmds.h"
#include "syscall.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

void cmd_rmmod(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: rmmod <name>\n");
        return;
    }

    while (*args == ' ') args++;

    char name[64];
    int ni = 0;
    while (*args && *args != ' ' && ni < (int)sizeof(name) - 1)
        name[ni++] = *args++;
    name[ni] = '\0';

    if (ni == 0) {
        kprintf("Usage: rmmod <name>\n");
        return;
    }

    int64_t ret = (int64_t)libc_syscall(SYS_DELETE_MODULE,
                                         (uint64_t)(uintptr_t)name,
                                         0, 0, 0, 0);

    if (ret == 0) {
        kprintf("rmmod: unloaded module '%s'\n", name);
    } else {
        int err = (int)(-ret);
        const char *msg = "Unknown error";
        switch (err) {
            case 2:  msg = "No such module"; break;             /* ENOENT */
            case 16: msg = "Module is busy (refcount > 0)"; break; /* EBUSY */
            case 1:  msg = "Operation not permitted"; break;    /* EPERM */
            default: break;
        }
        kprintf("rmmod: failed to unload '%s': %s\n", name, msg);
    }
}
