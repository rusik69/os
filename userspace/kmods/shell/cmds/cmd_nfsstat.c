#include "shell_cmds.h"
#include "printf.h"

void cmd_nfsstat(const char *args)
{
    (void)args;
    kprintf("NFS statistics:\n");
    kprintf("  RPC calls:    0\n");
    kprintf("  RPC errors:   0\n");
    kprintf("  NFS reads:    0\n");
    kprintf("  NFS writes:   0\n");
    kprintf("  NFS commits:  0\n");
}
