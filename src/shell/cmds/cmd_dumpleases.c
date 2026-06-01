/* cmd_dumpleases.c — show DHCP leases */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_dumpleases(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("dumpleases: DHCP lease information (stub — no leases active)\n");
    return 0;
}

void dumpleases_init(void)
{
    kprintf("[OK] cmd_dumpleases: DHCP lease display ready\n");
}
