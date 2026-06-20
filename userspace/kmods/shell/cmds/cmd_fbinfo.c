/* cmd_fbinfo.c — display backend and framebuffer details */
#include "vga.h"
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_fbinfo(void) {
    struct libc_fb_info info = {0};
    if (vga_get_fb_info(&info) < 0) {
        kprintf("Display info unavailable\n");
        return;
    }

    if (!info.is_framebuffer) {
        kprintf("Display backend: VGA text mode\n");
        kprintf("Console grid: %ux%u\n", VGA_WIDTH, VGA_HEIGHT);
        return;
    }

    kprintf("Display backend: framebuffer\n");
    kprintf("Framebuffer: %ux%u, %u bpp, pitch=%u bytes\n",
            (unsigned int)info.width, (unsigned int)info.height,
            (unsigned int)info.bpp, (unsigned int)info.pitch);
    kprintf("Console grid: %ux%u cells\n", VGA_WIDTH, VGA_HEIGHT);
}
