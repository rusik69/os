/* cmd_gzip.c — compress file using simple run-length encoding */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_gzip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: gzip <file>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char buf[4096];
    uint32_t size = 0;
    if (libc_vfs_read(path, buf, sizeof(buf), &size) != 0) {
        kprintf("gzip: %s: not found\n", path);
        return;
    }
    if (size == 0) { kprintf("gzip: empty file\n"); return; }

    /* Simple RLE: 0x00 marker, count, byte (for runs >= 4),
       otherwise literal bytes.
       Output format: magic "RLE1", then compressed data */

    static unsigned char out[8192];
    uint32_t opos = 0;
    out[opos++] = 'R'; out[opos++] = 'L'; out[opos++] = 'E'; out[opos++] = '1';

    uint32_t i = 0;
    while (i < size && opos < sizeof(out) - 256) {
        uint32_t run_start = i;
        while (i + 1 < size && buf[i + 1] == buf[run_start] && (i - run_start + 1) < 255)
            i++;
        uint32_t run_len = i - run_start + 1;
        if (run_len >= 4) {
            out[opos++] = 0x00;  /* escape/marker */
            out[opos++] = (unsigned char)run_len;
            out[opos++] = buf[run_start];
        } else {
            for (uint32_t k = 0; k < run_len; k++)
                out[opos++] = buf[run_start + k];
        }
        i++;
    }

    /* Write compressed output to file.gz */
    char outpath[128];
    strncpy(outpath, path, 64);
    int plen = strlen(outpath);
    if (plen < 120) { outpath[plen] = '.'; outpath[plen+1] = 'g'; outpath[plen+2] = 'z'; outpath[plen+3] = '\0'; }
    libc_vfs_write(outpath, out, opos);
    kprintf("Compressed %u -> %u bytes (%s)\n", (uint64_t)size, (uint64_t)opos, outpath);
}
