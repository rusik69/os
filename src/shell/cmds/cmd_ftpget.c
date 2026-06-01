/* cmd_ftpget.c — FTP download */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_ftpget(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: ftpget <host> <remote-file> [local-file]\n");
        return 1;
    }

    const char *host = argv[1];
    const char *remote = argv[2];
    const char *local = (argc >= 4) ? argv[3] : remote;

    kprintf("ftpget: downloading '%s' from %s to '%s' (stub)\n",
            remote, host, local);
    return 0;
}

void ftpget_init(void)
{
    kprintf("[OK] cmd_ftpget: FTP download command ready\n");
}
