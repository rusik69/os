/* cmd_od.c — Octal/hex dump of a file */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

static void print_octal(uint32_t val, int width) {
    char tmp[12];
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; }
    else {
        while (val > 0) { tmp[i++] = '0' + (val & 7); val >>= 3; }
    }
    /* Pad with zeros */
    while (i < width) tmp[i++] = '0';
    /* Print reversed */
    for (int j = i - 1; j >= 0; j--)
        kprintf("%c", (uint64_t)(uint8_t)tmp[j]);
}

void cmd_od(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: od <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4096, &size) != 0) {
        kprintf("od: cannot read '%s'\n", path);
        return;
    }

    /* Print in octal, 16 bytes per line */
    for (uint32_t i = 0; i < size; i += 16) {
        print_octal(i, 7);
        for (uint32_t j = 0; j < 16 && (i + j) < size; j++) {
            kprintf(" ");
            print_octal((uint8_t)buf[i + j], 3);
        }
        kprintf("\n");
    }
    print_octal(size, 7);
    kprintf("\n");
}
