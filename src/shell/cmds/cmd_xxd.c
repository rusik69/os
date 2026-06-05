/* cmd_xxd.c — xxd command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_xxd(const char *args) {
    if (!args) { kprintf("Usage: xxd <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/')
        snprintf(path, sizeof(path), "/%s", args);
    else
        snprintf(path, sizeof(path), "%s", args);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf), &size) < 0) {
        kprintf("Cannot read: %s\n", args);
        return;
    }
    if (size > 256) size = 256;
    for (uint32_t i = 0; i < size; i += 16) {
        kprintf("%08x: ", (unsigned int)i);
        for (int j = 0; j < 16; j++) {
            if (i + j < size)
                kprintf("%02x ", (unsigned int)(unsigned char)fbuf[i + j]);
            else
                kprintf("   ");
        }
        kprintf(" ");
        for (int j = 0; j < 16 && i + j < size; j++) {
            char c = fbuf[i + j];
            kprintf("%c", (unsigned int)(unsigned char)((c >= 32 && c < 127) ? c : '.'));
        }
        kprintf("\n");
    }
}
