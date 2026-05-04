/* cmd_write.c — write command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "ata.h"

void cmd_write(const char *args) {
    if (!args) { kprintf("Usage: write <file> <text>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *name = args;
    while (*args && *args != ' ') args++;
    if (!*args) { kprintf("Usage: write <file> <text>\n"); return; }
    char path[64];
    size_t nlen = args - name;
    if (nlen >= sizeof(path) - 2) nlen = sizeof(path) - 2;
    int off = 0;
    if (*name != '/') { path[0] = '/'; off = 1; }
    memcpy(path + off, name, nlen);
    path[off + nlen] = '\0';
    args++;
    if (fs_write_file(path, args, strlen(args)) < 0)
        kprintf("Write failed\n");
    else
        kprintf("Written %u bytes to %s\n", (uint64_t)strlen(args), path);
}
