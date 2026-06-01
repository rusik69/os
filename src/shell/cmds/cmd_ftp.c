/* cmd_ftp.c — Simple FTP client (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_ftp(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: ftp <host> [port]\n");
        return;
    }
    kprintf("ftp: not implemented\n");
}
