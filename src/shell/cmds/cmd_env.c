/* cmd_env.c — env command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_env(void) {
    uint8_t ip[4];
    libc_net_get_ip(ip);
    uint64_t ticks = libc_uptime_ticks();
    uint64_t sec = ticks / TIMER_FREQ;
    uint64_t pid = libc_getpid();
    kprintf("PID=%u\n", pid);
    kprintf("NAME=shell\n");
    kprintf("UPTIME=%u\n", sec);
    kprintf("IP=%u.%u.%u.%u\n",
            (uint64_t)ip[0], (uint64_t)ip[1],
            (uint64_t)ip[2], (uint64_t)ip[3]);
    kprintf("HOSTNAME=os-kernel\n");
    kprintf("DISK=%s\n", ata_is_present() ? "yes" : "no");
    kprintf("NET=%s\n", libc_net_is_present() ? "yes" : "no");
}
