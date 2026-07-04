/* rmmod.c — unload kernel module with dependency check (D234)
 *
 * Usage:
 *   rmmod <module>          — unload module
 *   rmmod -f <module>       — force unload (allows even with refcount > 0)
 *
 * The kernel's sys_delete_module performs dependency checking:
 *   - Returns -EBUSY if module has active dependents
 *   - Returns -ENOENT if module not found
 *   - Returns -EPERM if not privileged
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[])
{
    int flags = 0;
    const char *mod_name = NULL;

    if (argc < 2) {
        printf("Usage: rmmod [-f] <module>\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            flags |= 1; /* O_NONBLOCK — allow force unload */
        } else {
            mod_name = argv[i];
        }
    }

    if (!mod_name) {
        printf("rmmod: missing module name\n");
        return 1;
    }

    if (delete_module(mod_name, flags) < 0) {
        printf("rmmod: cannot remove %s\n", mod_name);
        return 1;
    }
    return 0;
}
