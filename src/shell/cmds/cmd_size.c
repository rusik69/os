/* cmd_size.c — print size of file (simplified ELF section analysis — just file size) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_size(const char *args) {
    if (!args || !*args) { kprintf("Usage: size <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); path[63] = '\0'; }
    else strncpy(path, args, 63);
    path[63] = '\0';

    uint32_t size;
    uint8_t type;
    if (fs_stat(path, &size, &type) < 0) {
        kprintf("size: cannot stat '%s'\n", args);
        shell_set_exit_status(1);
        return;
    }

    kprintf("text\tdata\tbss\tdec\thex\tfilename\n");
    kprintf("%u\t0\t0\t%u\t%x\t%s\n",
            size, size, size, args);
}
