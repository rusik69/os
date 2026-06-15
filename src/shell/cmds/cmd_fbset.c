/* cmd_fbset.c — framebuffer settings */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "fbcon.h"

int cmd_fbset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t *fb = NULL;
    uint32_t w = 0, h = 0, pitch = 0;

    fbcon_get_fb(&fb, &w, &h, &pitch);

    if (!fb || w == 0 || h == 0) {
        kprintf("fbset: framebuffer not initialized\n");
        return 1;
    }

    /* Calculate bits per pixel from pitch */
    uint32_t bpp = (pitch / w) * 8;

    kprintf("framebuffer information:\n");
    kprintf("  geometry: %u x %u\n", (unsigned int)w, (unsigned int)h);
    kprintf("  bpp:      %u\n", (unsigned int)bpp);
    kprintf("  pitch:    %u bytes/scanline\n", (unsigned int)pitch);
    kprintf("  size:     %u bytes\n", (unsigned int)(pitch * h));
    kprintf("  addr:     0x%p\n", (void *)fb);
    kprintf("  console:  %u cols x %u rows\n", FBCON_COLS, FBCON_ROWS);

    return 0;
}

void fbset_init(void)
{
    kprintf("[OK] cmd_fbset: framebuffer settings ready\n");
}
