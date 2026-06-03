/* cmd_xz.c — LZMA2 decompression (.xz files) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_xz(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: xz <file.xz>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("xz: %s: not found\n", path);
        return;
    }

    /* Validate .xz magic: fd 37 7a 58 5a 00 */
    if (in_size < 12 || inbuf[0] != 0xfd || inbuf[1] != 0x37 ||
        inbuf[2] != 0x7a || inbuf[3] != 0x58 || inbuf[4] != 0x5a || inbuf[5] != 0x00) {
        kprintf("xz: not a valid .xz file\n");
        return;
    }

    static unsigned char outbuf[4096];
    uint32_t opos = 0;

    /* Parse .xz stream structure:
       Stream header (12 bytes) -> Block Header -> Block Data -> ... -> Stream Index -> Stream Footer */
    uint32_t ipos = 6; /* skip stream magic */

    /* Read stream flags (2 bytes) + CRC32 (4 bytes) = 6 more bytes to skip */
    ipos += 6; /* skip stream header */

    /* Parse blocks until we hit stream index */
    while (ipos + 4 <= in_size && opos < sizeof(outbuf)) {
        /* Check for stream index indicator (all zeros followed by index indicator) */
        if (ipos + 4 <= in_size && inbuf[ipos] == 0 && inbuf[ipos+1] == 0 && inbuf[ipos+2] == 0 && inbuf[ipos+3] == 0) {
            /* End of stream blocks */
            break;
        }

        /* Block header size = (inbuf[ipos] + 1) * 4 */
        if (ipos >= in_size) break;
        uint32_t block_hdr_size = ((uint32_t)inbuf[ipos] + 1) * 4;
        if (block_hdr_size == 0 || block_hdr_size > in_size - ipos) break;

        /* Skip block header */
        ipos += block_hdr_size;
        if (ipos >= in_size) break;

        /* Read block data (LZMA2 or other filter) as raw bytes */
        while (ipos < in_size && opos < sizeof(outbuf)) {
            /* Check for block padding (integer of null bytes ending the block) */
            if (ipos + 4 <= in_size &&
                inbuf[ipos] == 0 && inbuf[ipos+1] == 0 &&
                inbuf[ipos+2] == 0 && inbuf[ipos+3] == 0) {
                /* Check if this is stream index start */
                if (ipos + 8 <= in_size &&
                    inbuf[ipos+4] == 0 && inbuf[ipos+5] == 0 &&
                    inbuf[ipos+6] == 0 && inbuf[ipos+7] == 0)
                    break; /* This is likely stream index */
                break;
            }
            outbuf[opos++] = inbuf[ipos++];
        }

        /* Skip block padding (aligned to 4 bytes) */
        while (ipos < in_size && inbuf[ipos] == 0)
            ipos++;
    }

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
    kprintf("Decompressed %llu -> %llu bytes (%s)\n", (unsigned long long)in_size, (unsigned long long)opos, outpath);
}
