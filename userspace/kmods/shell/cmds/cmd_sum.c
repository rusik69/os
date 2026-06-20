/* cmd_sum.c — checksum and count blocks (SysV sum) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

/* Rotating checksum used by SysV sum */
static unsigned short sysv_checksum(const unsigned char *data, uint32_t len) {
    unsigned short s = 0;
    for (uint32_t i = 0; i < len; i++) {
        s = (s + data[i]) & 0xFFFF;
        s = ((s << 1) | (s >> 15)) & 0xFFFF;
    }
    return s;
}

void cmd_sum(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: sum <file>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static unsigned char buf[8192];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("sum: cannot read '%s'\n", path);
        return;
    }
    unsigned short cksum = sysv_checksum(buf, size);
    int blocks = (size + 511) / 512;
    kprintf("%05u %5d %s\n", (unsigned int)cksum, blocks, path);
}
