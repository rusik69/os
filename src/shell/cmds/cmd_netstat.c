#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_netstat(const char *args) {
    (void)args;
    kprintf("Network interfaces:\n");
    extern void net_get_ip(unsigned char *ip);
    unsigned char ip[4];
    net_get_ip(ip);
    kprintf("  IP: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
}
