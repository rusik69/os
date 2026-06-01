/* cmd_yes.c -- Repeatedly output 'y' or a given string */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_yes(int argc, char **argv) {
    const char *str = "y";
    if (argc >= 2)
        str = argv[1];

    for (;;) {
        kprintf("%s\n", str);
    }
    return 0; /* unreachable */
}
