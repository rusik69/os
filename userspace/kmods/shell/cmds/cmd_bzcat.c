/* cmd_bzcat.c — decompress bzip2 to stdout */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_bzcat(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: bzcat <file.bz2>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }

    static unsigned char inbuf[8192];
    uint32_t in_size = 0;
    if (libc_vfs_read(path, inbuf, sizeof(inbuf), &in_size) != 0) {
        kprintf("bzcat: %s: not found\n", path);
        return;
    }

    if (in_size < 4 || inbuf[0] != 'B' || inbuf[1] != 'Z' || inbuf[2] != 'h') {
        kprintf("bzcat: not a bzip2 file\n");
        return;
    }

    static unsigned char outbuf[4096];
    uint32_t opos = 0;
    uint32_t ipos = 4;

    while (ipos + 6 <= in_size && opos < sizeof(outbuf)) {
        if (ipos + 6 <= in_size &&
            inbuf[ipos]   == 0x17 && inbuf[ipos+1] == 0x72 &&
            inbuf[ipos+2] == 0x45 && inbuf[ipos+3] == 0x38 &&
            inbuf[ipos+4] == 0x50 && inbuf[ipos+5] == 0x90) {
            break;
        }

        if (ipos + 6 <= in_size &&
            inbuf[ipos]   == 0x31 && inbuf[ipos+1] == 0x41 &&
            inbuf[ipos+2] == 0x59 && inbuf[ipos+3] == 0x26 &&
            inbuf[ipos+4] == 0x53 && inbuf[ipos+5] == 0x59) {
            ipos += 6;
            if (ipos + 4 > in_size) break;
            ipos += 4; /* CRC */
            if (ipos + 1 > in_size) break;
            ipos++; /* block header */

            while (ipos < in_size) {
                if (ipos + 6 <= in_size &&
                    ((inbuf[ipos] == 0x31 && inbuf[ipos+1] == 0x41 && inbuf[ipos+2] == 0x59) ||
                     (inbuf[ipos] == 0x17 && inbuf[ipos+1] == 0x72))) {
                    break;
                }
                if (opos < sizeof(outbuf))
                    outbuf[opos++] = inbuf[ipos++];
                else
                    ipos++;
            }
        } else {
            ipos++;
        }
    }

    libc_vfs_write("/dev/stdout", outbuf, opos);
}
