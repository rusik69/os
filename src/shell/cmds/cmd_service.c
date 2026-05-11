/* cmd_service.c — 'service' shell command: start/stop/status/list services */

#include "shell_cmds.h"
#include "service.h"
#include "printf.h"
#include "string.h"

void cmd_service(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: service <start|stop|status|list> [name]\n");
        return;
    }

    /* Parse subcommand and optional service name */
    char subcmd[16] = {0};
    char name[SERVICE_NAME_MAX] = {0};

    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(subcmd) - 1)
        subcmd[i++] = *p++;
    while (*p == ' ') p++;
    i = 0;
    while (*p && i < (int)sizeof(name) - 1)
        name[i++] = *p++;

    if (strcmp(subcmd, "list") == 0) {
        int n = service_count();
        if (n == 0) {
            kprintf("No services registered.\n");
            return;
        }
        kprintf("%-16s  STATUS\n", "NAME");
        kprintf("%-16s  ------\n", "----");
        for (int j = 0; j < n; j++) {
            struct service *svc = service_get(j);
            if (svc)
                kprintf("%-16s  %s\n", svc->name,
                        svc->state == SERVICE_RUNNING ? "running" : "stopped");
        }
        return;
    }

    if (strcmp(subcmd, "status") == 0) {
        if (*name == '\0') {
            /* No name → same as list */
            cmd_service("list");
            return;
        }
        struct service *svc = service_find(name);
        if (!svc) { kprintf("service: unknown service '%s'\n", name); return; }
        kprintf("%s: %s\n", svc->name,
                svc->state == SERVICE_RUNNING ? "running" : "stopped");
        return;
    }

    if (strcmp(subcmd, "start") == 0) {
        if (*name == '\0') { kprintf("service start: missing name\n"); return; }
        service_start(name);
        return;
    }

    if (strcmp(subcmd, "stop") == 0) {
        if (*name == '\0') { kprintf("service stop: missing name\n"); return; }
        service_stop(name);
        return;
    }

    kprintf("service: unknown subcommand '%s'\n", subcmd);
    kprintf("Usage: service <start|stop|status|list> [name]\n");
}
