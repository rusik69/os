/* cmd_chvt.c — change virtual terminal */
#include "vga.h"
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

int cmd_chvt(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: chvt <terminal_number>\n");
        return 1;
    }
    int vt = atoi(argv[1]);
    if (vt < 0 || vt > 15) {
        kprintf("chvt: invalid terminal number %d (0-15)\n", vt);
        return 1;
    }
    vga_set_cursor(0, (uint16_t)vt);
    return 0;
}
