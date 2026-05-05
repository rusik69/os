/* cmd_strings.c — strings: extract printable ASCII strings from a file */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"

#define STRINGS_MIN_LEN 4

void cmd_strings(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: strings <file>\n");
        kprintf("  Print runs of >= %d printable characters\n", (uint64_t)STRINGS_MIN_LEN);
        return;
    }

    char path[64];
    if (args[0] != '/') {
        path[0] = '/';
        strncpy(path + 1, args, 62);
        path[63] = '\0';
    } else {
        strncpy(path, args, 63);
        path[63] = '\0';
    }

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("strings: %s: not found\n", path);
        return;
    }
    buf[size] = '\0';

    char str[256];
    int slen = 0;
    for (uint32_t i = 0; i <= size; i++) {
        unsigned char c = (i < size) ? (unsigned char)buf[i] : 0;
        if (c >= 32 && c < 127) {
            if (slen < 255) str[slen++] = (char)c;
        } else {
            if (slen >= STRINGS_MIN_LEN) {
                str[slen] = '\0';
                kprintf("%s\n", str);
            }
            slen = 0;
        }
    }
}
