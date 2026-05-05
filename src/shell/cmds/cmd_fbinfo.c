/* cmd_fbinfo.c — display backend and framebuffer details */
#include "shell_cmds.h"
#include "printf.h"
#include "vga.h"

void cmd_fbinfo(void) {
    uint32_t w = 0, h = 0, pitch = 0;
    uint8_t bpp = 0;

    if (!vga_is_framebuffer()) {
        kprintf("Display backend: VGA text mode\n");
        kprintf("Console grid: %ux%u\n", (uint64_t)VGA_WIDTH, (uint64_t)VGA_HEIGHT);
        return;
    }

    vga_get_framebuffer_info(&w, &h, &pitch, &bpp);
    kprintf("Display backend: framebuffer\n");
    kprintf("Framebuffer: %ux%u, %u bpp, pitch=%u bytes\n",
            (uint64_t)w, (uint64_t)h, (uint64_t)bpp, (uint64_t)pitch);
    kprintf("Console grid: %ux%u cells\n", (uint64_t)VGA_WIDTH, (uint64_t)VGA_HEIGHT);
}
