/* cmd_reset.c — Reset terminal: clear screen and reset colors */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vga.h"
#include "libc.h"

void cmd_reset(const char *args) {
    (void)args;

    /* Clear screen */
    libc_vga_clear();

    /* Reset colors to white on black */
    libc_vga_set_color(VGA_WHITE, VGA_BLACK);

    kprintf("Terminal reset.\n");
}
