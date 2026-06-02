/* cmd_insmod.c — Load a kernel module (M21)
 *
 * Usage: insmod <path> [param=val ...]
 *
 * Calls the SYS_INIT_MODULE syscall to load a .ko ELF file
 * from the filesystem and optionally set module parameters.
 */
#include "shell_cmds.h"
#include "syscall.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

void cmd_insmod(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: insmod <path> [param=val ...]\n");
        return;
    }

    /* Parse the first token as path */
    char path[128];
    const char *p = args;
    while (*p == ' ') p++;
    int pi = 0;
    while (*p && *p != ' ' && pi < (int)sizeof(path) - 1)
        path[pi++] = *p++;
    path[pi] = '\0';

    /* Rest is parameter string (if any) */
    while (*p == ' ') p++;
    const char *params = (*p) ? p : NULL;

    /* Call sys_init_module(path, params) via libc_syscall */
    int64_t ret = (int64_t)libc_syscall(SYS_INIT_MODULE,
                                         (uint64_t)(uintptr_t)path,
                                         params ? (uint64_t)(uintptr_t)params : 0,
                                         0, 0, 0);

    if (ret >= 0) {
        kprintf("insmod: loaded module '%s' (id=%lld)\n", path, (long long)ret);
    } else {
        int err = (int)(-ret);
        const char *msg = "Unknown error";
        switch (err) {
            case 2:  msg = "No such file or directory"; break;  /* ENOENT */
            case 5:  msg = "I/O error"; break;                  /* EIO */
            case 12: msg = "Out of memory"; break;              /* ENOMEM */
            case 22: msg = "Invalid argument"; break;           /* EINVAL */
            case 27: msg = "File too large"; break;             /* EFBIG */
            case 63: msg = "File is not executable"; break;     /* ENOEXEC */
            case 1:  msg = "Operation not permitted"; break;    /* EPERM */
            default: break;
        }
        kprintf("insmod: failed to load '%s': %s\n", path, msg);
    }
}
