/* service.c — Service management stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: service <name> <start|stop|status>\n");
        return 1;
    }
    const char *name = argv[1];
    const char *cmd = argv[2];
    if (strcmp(cmd, "start") == 0)
        printf("service: starting %s (stub)\n", name);
    else if (strcmp(cmd, "stop") == 0)
        printf("service: stopping %s (stub)\n", name);
    else if (strcmp(cmd, "status") == 0)
        printf("service: %s is unknown (stub)\n", name);
    else
        printf("service: unknown command '%s'\n", cmd);
    return 0;
}
