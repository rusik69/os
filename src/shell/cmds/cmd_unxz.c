/* cmd_unxz.c — decompress .xz files */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_unxz(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unxz <file.xz>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("unxz: %s: not found\n", path);
        return;
    }

    /* Validate .xz magic */
    if (in_size < 12 || inbuf[0] != 0xfd || inbuf[1] != 0x37 ||
        inbuf[2] != 0x7a || inbuf[3] != 0x58 || inbuf[4] != 0x5a || inbuf[5] != 0x00) {
        kprintf("unxz: not a valid .xz file\n");
        return;
    }

    static unsigned char outbuf[4096];
    uint32_t opos = 0;
    uint32_t ipos = 12; /* skip stream header (6 magic + 2 flags + 4 crc) */

    while (ipos + 4 <= in_size && opos < sizeof(outbuf)) {
        /* Check for stream index marker */
        if (inbuf[ipos] == 0 && inbuf[ipos+1] == 0 && inbuf[ipos+2] == 0 && inbuf[ipos+3] == 0)
            break;

        /* Block header size */
        uint32_t bhs = ((uint32_t)inbuf[ipos] + 1) * 4;
        if (bhs == 0 || bhs > in_size - ipos) break;
        ipos += bhs;
        if (ipos >= in_size) break;

        /* Copy block data until padding */
        while (ipos < in_size && opos < sizeof(outbuf)) {
            if (ipos + 4 <= in_size && inbuf[ipos] == 0 && inbuf[ipos+1] == 0 &&
                inbuf[ipos+2] == 0 && inbuf[ipos+3] == 0) {
                /* Skip padding */
                while (ipos < in_size && inbuf[ipos] == 0) {
                    if (ipos + 8 <= in_size && inbuf[ipos] == 0 && inbuf[ipos+1] == 0 &&
                        inbuf[ipos+2] == 0 && inbuf[ipos+3] == 0 &&
                        inbuf[ipos+4] == 0 && inbuf[ipos+5] == 0 &&
                        inbuf[ipos+6] == 0 && inbuf[ipos+7] == 0)
                        goto done_blocks;
                    ipos++;
                }
                break;
            }
            outbuf[opos++] = inbuf[ipos++];
        }
    }
done_blocks:

    char outpath[128];
    strncpy(outpath, path, 120);
    int plen = strlen(outpath);
    if (plen > 3 && outpath[plen-3] == '.' && outpath[plen-2] == 'x' && outpath[plen-1] == 'z')
        outpath[plen-3] = '\0';
    else {
        int len = strlen(outpath);
        if (len < 120) { outpath[len] = '.'; outpath[len+1] = 'o'; outpath[len+2] = 'u'; outpath[len+3] = 't'; outpath[len+4] = '\0'; }
    }

    libc_vfs_write(outpath, outbuf, opos);
    kprintf("Decompressed %u -> %u bytes (%s)\n", (unsigned long)in_size, (unsigned long)opos, outpath);
}
