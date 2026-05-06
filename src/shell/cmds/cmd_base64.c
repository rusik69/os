/* cmd_base64.c — base64 encode a file */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void cmd_base64(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: base64 <file>\n");
        kprintf("  Encode file contents as base64\n");
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

    /* Must be a multiple of 3 to keep encoding clean */
    static unsigned char buf[3072];
    uint32_t size = 0;
    if (vfs_read(path, (char *)buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("base64: %s: not found\n", path);
        return;
    }

    int col = 0;
    for (uint32_t i = 0; i < size; i += 3) {
        unsigned char b0 = buf[i];
        unsigned char b1 = (i + 1 < size) ? buf[i + 1] : 0;
        unsigned char b2 = (i + 2 < size) ? buf[i + 2] : 0;

        kprintf("%c", (uint64_t)b64chars[b0 >> 2]);
        kprintf("%c", (uint64_t)b64chars[((b0 & 0x03) << 4) | (b1 >> 4)]);
        kprintf("%c", (uint64_t)(i + 1 < size ? b64chars[((b1 & 0x0F) << 2) | (b2 >> 6)] : '='));
        kprintf("%c", (uint64_t)(i + 2 < size ? b64chars[b2 & 0x3F] : '='));

        col += 4;
        if (col >= 76) {
            kprintf("\n");
            col = 0;
        }
    }
    if (col > 0) kprintf("\n");
}
