/* cmd_ftpput.c — FTP upload */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_ftpput(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: ftpput <host> <local-file> [remote-file]\n");
        return 1;
    }

    const char *host = argv[1];
    const char *local = argv[2];
    const char *remote = (argc >= 4) ? argv[3] : local;

    kprintf("ftpput: uploading '%s' to %s as '%s' (stub)\n",
            local, host, remote);
    return 0;
}

void ftpput_init(void)
{
    kprintf("[OK] cmd_ftpput: FTP upload command ready\n");
}
