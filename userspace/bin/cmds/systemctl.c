/* systemctl.c — System control stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: systemctl <start|stop|restart|status> <service>\n");
        return 1;
    }
    const char *cmd = argv[1];
    const char *svc = argc >= 3 ? argv[2] : "";
    if (strcmp(cmd, "start") == 0)
        printf("systemctl: starting %s (stub)\n", svc);
    else if (strcmp(cmd, "stop") == 0)
        printf("systemctl: stopping %s (stub)\n", svc);
    else if (strcmp(cmd, "restart") == 0)
        printf("systemctl: restarting %s (stub)\n", svc);
    else if (strcmp(cmd, "status") == 0)
        printf("systemctl: %s is unknown (stub)\n", svc);
    else if (strcmp(cmd, "list-units") == 0)
        printf("systemctl: no units (stub)\n");
    else
        printf("systemctl: unknown command '%s'\n", cmd);
    return 0;
}
