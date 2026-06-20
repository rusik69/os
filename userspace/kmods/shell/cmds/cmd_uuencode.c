/* cmd_uuencode.c — encode binary to text (simple uuencode-like output) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static const char uu_tbl[] =
    "`!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_";

void cmd_uuencode(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: uuencode <file> [name]\n");
        return;
    }
    char path[64];
    char name[64] = "data";
    /* Parse args: first token is file path, second optional name */
    int i = 0;
    while (args[i] == ' ') i++;
    int j = 0;
    while (args[i] && args[i] != ' ' && j < 63) path[j++] = args[i++];
    path[j] = '\0';
    while (args[i] == ' ') i++;
    if (args[i]) {
        j = 0;
        while (args[i] && j < 63) name[j++] = args[i++];
        name[j] = '\0';
    }
    /* Prepend / if not absolute */
    char fullpath[128];
    if (path[0] != '/') { fullpath[0] = '/'; strncpy(fullpath + 1, path, 125); fullpath[126] = '\0'; }
    else strncpy(fullpath, path, 127);

    static unsigned char buf[3072];
    uint32_t size = 0;
    if (libc_vfs_read(fullpath, buf, sizeof(buf), &size) != 0) {
        kprintf("uuencode: %s: not found\n", path);
        return;
    }
    kprintf("begin 644 %s\n", name);
    uint32_t off = 0;
    while (off < size) {
        int chunk = (size - off < 45) ? size - off : 45;
        kprintf("%c", (unsigned int)(unsigned char)uu_tbl[chunk]);
        for (int k = 0; k < chunk; k += 3) {
            unsigned char b0 = buf[off + k];
            unsigned char b1 = (k + 1 < chunk) ? buf[off + k + 1] : 0;
            unsigned char b2 = (k + 2 < chunk) ? buf[off + k + 2] : 0;
            kprintf("%c%c%c%c",
                (unsigned int)(unsigned char)uu_tbl[b0 >> 2],
                (unsigned int)(unsigned char)uu_tbl[((b0 & 0x03) << 4) | (b1 >> 4)],
                (unsigned int)(unsigned char)uu_tbl[((b1 & 0x0F) << 2) | (b2 >> 6)],
                (unsigned int)(unsigned char)uu_tbl[b2 & 0x3F]);
        }
        kprintf("\n");
        off += chunk;
    }
    kprintf("`\nend\n");
}
