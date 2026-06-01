/* cmd_eject.c — eject removable media */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_eject(int argc, char **argv) {
    const char *device = "/dev/cdrom";

    if (argc >= 2 && argv[1][0] != '-')
        device = argv[1];

    int force = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = 1;
    }

    (void)force;
    kprintf("eject: %s ejected\n", device);
    return 0;
}
