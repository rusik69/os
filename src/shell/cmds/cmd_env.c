/* cmd_env.c — env command */
#include "shell_cmds.h"
#include "printf.h"
#include "net.h"
#include "timer.h"
#include "process.h"
#include "ata.h"
#include "e1000.h"

void cmd_env(void) {
    uint8_t ip[4];
    net_get_ip(ip);
    uint64_t ticks = timer_get_ticks();
    uint64_t sec = ticks / TIMER_FREQ;
    struct process *p = process_get_current();
    kprintf("PID=%u\n", (uint64_t)p->pid);
    kprintf("NAME=%s\n", p->name);
    kprintf("UPTIME=%u\n", sec);
    kprintf("IP=%u.%u.%u.%u\n",
            (uint64_t)ip[0], (uint64_t)ip[1],
            (uint64_t)ip[2], (uint64_t)ip[3]);
    kprintf("HOSTNAME=os-kernel\n");
    kprintf("DISK=%s\n", ata_is_present() ? "yes" : "no");
    kprintf("NET=%s\n", e1000_is_present() ? "yes" : "no");
}
