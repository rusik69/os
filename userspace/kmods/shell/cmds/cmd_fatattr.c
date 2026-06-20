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
    if (attr & 0x01) buf[pos++] = 'R';
    if (attr & 0x02) buf[pos++] = 'H';
    if (attr & 0x04) buf[pos++] = 'S';
    if (attr & 0x08) buf[pos++] = 'V';
    if (attr & 0x10) buf[pos++] = 'D';
    if (attr & 0x20) buf[pos++] = 'A';
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
        struct vfs_stat st;
        if (vfs_stat(argv[i], &st) >= 0) {
            uint8_t attr = 0;
            if (st.type == FS_TYPE_DIR)  attr |= 0x10; /* directory */
            if (st.type == FS_TYPE_FILE) attr |= 0x20; /* archive */
            if (!(st.mode & 0200))        attr |= 0x01; /* read-only */
            kprintf("fatattr: %s attributes = %s (0x%02x)\n",
                    argv[i], attr_str(attr), (unsigned int)attr);
        } else {
            kprintf("fatattr: cannot access '%s'\n", argv[i]);
        }
    }
    return 0;
}

void fatattr_init(void)
{
    kprintf("[OK] cmd_fatattr: FAT attribute viewer ready\n");
}
