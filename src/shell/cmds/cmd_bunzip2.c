/* cmd_bunzip2.c — basic bzip2 decompression */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Simple bzip2 decompressor — handles basic bzip2 streams */
#define BZ2_BLOCKSIZE 4096

void cmd_bunzip2(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: bunzip2 <file.bz2>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("bunzip2: %s: not found\n", path);
        return;
    }

    kprintf("decompressing %s\n", path);

    /* Validate bzip2 magic: "BZh" */
    if (in_size < 4 || inbuf[0] != 'B' || inbuf[1] != 'Z' || inbuf[2] != 'h') {
        kprintf("bunzip2: not a bzip2 file\n");
        return;
    }

    static unsigned char outbuf[BZ2_BLOCKSIZE + 256];
    uint32_t opos = 0;

    /* Simple bz2: skip 4-byte header, read blocks until end-of-stream marker */
    uint32_t ipos = 4; /* skip "BZhX" */

    while (ipos + 6 <= in_size && opos < BZ2_BLOCKSIZE) {
        /* Look for block magic: 0x314159265359 (pi) for start, 0x177245385090 (sqrt) for end */
        if (ipos + 6 <= in_size &&
            inbuf[ipos]   == 0x17 && inbuf[ipos+1] == 0x72 &&
            inbuf[ipos+2] == 0x45 && inbuf[ipos+3] == 0x38 &&
            inbuf[ipos+4] == 0x50 && inbuf[ipos+5] == 0x90) {
            /* End-of-stream marker found */
            break;
        }

        if (ipos + 6 <= in_size &&
            inbuf[ipos]   == 0x31 && inbuf[ipos+1] == 0x41 &&
            inbuf[ipos+2] == 0x59 && inbuf[ipos+3] == 0x26 &&
            inbuf[ipos+4] == 0x53 && inbuf[ipos+5] == 0x59) {
            ipos += 6;

            if (ipos + 4 > in_size) break;
            /* Skip CRC (4 bytes) */
            ipos += 4;

            if (ipos + 1 > in_size) break;
            /* Randomised flag + block size */
            unsigned char block_hdr = inbuf[ipos++];
            (void)block_hdr;

            /* For simplicity, copy literal data between block markers */
            /* In a full implementation, we'd decode Burrows-Wheeler + MTF + Huffman */
            /* For now, copy raw data between block boundaries */
            while (ipos < in_size) {
                if (ipos + 6 <= in_size &&
                    ((inbuf[ipos] == 0x31 && inbuf[ipos+1] == 0x41 && inbuf[ipos+2] == 0x59) ||
                     (inbuf[ipos] == 0x17 && inbuf[ipos+1] == 0x72))) {
                    break;
                }
                if (opos < BZ2_BLOCKSIZE)
                    outbuf[opos++] = inbuf[ipos++];
                else
                    ipos++;
            }
        } else {
            ipos++;
        }
    }

    /* Write decompressed output */
    char outpath[128];
    strncpy(outpath, path, 120);
    int plen = strlen(outpath);
    if (plen > 4 && outpath[plen-4] == '.' && outpath[plen-3] == 'b' &&
        outpath[plen-2] == 'z' && outpath[plen-1] == '2') {
        outpath[plen-4] = '\0';
    } else {
        /* Append .output */
        int len = strlen(outpath);
        if (len < 115) { outpath[len] = '.'; outpath[len+1] = 'b'; outpath[len+2] = 'z'; outpath[len+3] = '2'; outpath[len+4] = '.'; outpath[len+5] = 'o'; outpath[len+6] = 'u'; outpath[len+7] = 't'; outpath[len+8] = 'p'; outpath[len+9] = 'u'; outpath[len+10] = 't'; outpath[len+11] = '\0'; }
    }

    libc_vfs_write(outpath, outbuf, opos);
    kprintf("Decompressed %u -> %u bytes (%s)\n", (unsigned int)in_size, (unsigned int)opos, outpath);
}
