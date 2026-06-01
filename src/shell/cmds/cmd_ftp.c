#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_ftp(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: ftp <args>\n");
        return;
    }
    kprintf("ftp: %s\n", args);
}
