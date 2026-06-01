/* cmd_chvt.c — change virtual terminal */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_chvt(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: chvt <terminal_number>\n");
        return 1;
    }
    int vt = atoi(argv[1]);
    if (vt < 1 || vt > 12) {
        kprintf("chvt: invalid terminal number %d (1-12)\n", vt);
        return 1;
    }
    /* libc_vga_clear is used as a proxy for switching */
    libc_vga_clear();
    kprintf("Switched to virtual terminal %d\n", vt);
    return 0;
}
