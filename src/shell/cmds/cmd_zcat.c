/* cmd_zcat.c — decompress gzip to stdout (stub using RLE1 decompression) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_zcat(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: zcat <file.gz>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char buf[8192];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("zcat: %s: not found\n", path);
        return;
    }
    if (size < 4 || buf[0] != 'R' || buf[1] != 'L' || buf[2] != 'E' || buf[3] != '1') {
        kprintf("zcat: not in RLE1 format\n");
        return;
    }

    static unsigned char out[4096];
    uint32_t opos = 0;
    uint32_t i = 4;
    while (i < size && opos < sizeof(out)) {
        if (buf[i] == 0x00 && i + 2 < size) {
            unsigned char run_len = buf[i + 1];
            unsigned char byte_val = buf[i + 2];
            for (int k = 0; k < run_len && opos < sizeof(out); k++)
                out[opos++] = byte_val;
            i += 3;
        } else {
            out[opos++] = buf[i++];
        }
    }
    /* Write output to stdout by printing as string (may contain binary) */
    libc_vfs_write("/dev/stdout", out, opos);
}
