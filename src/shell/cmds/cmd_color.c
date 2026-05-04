/* cmd_color.c — color command */
#include "shell_cmds.h"
#include "printf.h"
#include "vga.h"

void cmd_color(const char *args) {
    if (!args) { kprintf("Usage: color <fg> [bg] (0-15)\n"); return; }
    uint8_t fg = 0, bg = 0;
    while (*args >= '0' && *args <= '9') { fg = fg * 10 + (*args - '0'); args++; }
    while (*args == ' ') args++;
    if (*args >= '0' && *args <= '9') {
        while (*args >= '0' && *args <= '9') { bg = bg * 10 + (*args - '0'); args++; }
    }
    if (fg > 15) fg = 15;
    if (bg > 15) bg = 15;
    vga_set_color(fg, bg);
    kprintf("Color set to %u on %u\n", (uint64_t)fg, (uint64_t)bg);
}
