/* cmd_fatattr.c — FAT attributes */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

static const char *attr_str(uint8_t attr)
{
    static char buf[16];
    int pos = 0;

    if (attr & 0x01) buf[pos++] = 'R'; /* read-only */
    if (attr & 0x02) buf[pos++] = 'H'; /* hidden */
    if (attr & 0x04) buf[pos++] = 'S'; /* system */
    if (attr & 0x08) buf[pos++] = 'V'; /* volume label */
    if (attr & 0x10) buf[pos++] = 'D'; /* directory */
    if (attr & 0x20) buf[pos++] = 'A'; /* archive */
    buf[pos] = '\0';
    return buf;
}

int cmd_fatattr(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: fatattr <file> [file...]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        kprintf("fatattr: %s attributes = %s (stub)\n",
                argv[i], attr_str(0x20));
    }
    return 0;
}

void fatattr_init(void)
{
    kprintf("[OK] cmd_fatattr: FAT attribute viewer ready\n");
}
