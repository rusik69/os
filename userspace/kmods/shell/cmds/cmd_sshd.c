/* cmd_sshd.c — SSH daemon control */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "ssh.h"
#include "service.h"

void cmd_sshd(const char *args) {
    if (!args || *args == '\0') {
        struct service *svc = service_find("sshd");
        if (svc && svc->state == SERVICE_RUNNING) {
            kprintf("sshd: running (port 22)\n");
        } else {
            kprintf("sshd: stopped\n");
        }
        return;
    }

    while (*args == ' ') args++;

    if (strcmp(args, "start") == 0) {
        if (service_start("sshd") == 0)
            kprintf("sshd: started on port 22\n");
        else
            kprintf("sshd: failed to start\n");
    } else if (strcmp(args, "stop") == 0) {
        service_stop("sshd");
        kprintf("sshd: stopped\n");
    } else if (strcmp(args, "status") == 0) {
        struct service *svc = service_find("sshd");
        if (svc && svc->state == SERVICE_RUNNING)
            kprintf("sshd: running (port 22)\n");
        else
            kprintf("sshd: stopped\n");
    } else if (strcmp(args, "restart") == 0) {
        service_stop("sshd");
        if (service_start("sshd") == 0)
            kprintf("sshd: restarted on port 22\n");
        else
            kprintf("sshd: failed to restart\n");
    } else {
        kprintf("Usage: sshd [start|stop|status|restart]\n");
    }
}
