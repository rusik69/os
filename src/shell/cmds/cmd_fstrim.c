/* cmd_fstrim.c — discard unused blocks */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_fstrim(int argc, char **argv)
{
    const char *mountpoint = "/";
    uint64_t minlen = 0;

    if (argc >= 2)
        mountpoint = argv[1];
    if (argc >= 3)
        minlen = (uint64_t)strtoul(argv[2], NULL, 10);

    kprintf("fstrim: trimming '%s' minlen=%llu (stub — %llu bytes discarded)\n",
            mountpoint, minlen, minlen ? 0ULL : 1024ULL * 1024ULL);
    return 0;
}

void fstrim_init(void)
{
    kprintf("[OK] cmd_fstrim: filesystem trim command ready\n");
}
