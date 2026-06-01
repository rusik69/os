/* cmd_ionice.c — I/O priority */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

static const char *class_name(int cls)
{
    switch (cls) {
        case 0: return "none";
        case 1: return "realtime";
        case 2: return "best-effort";
        case 3: return "idle";
        default: return "unknown";
    }
}

int cmd_ionice(int argc, char **argv)
{
    int cls = 2;  /* best-effort */
    int prio = 4;
    int pid = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cls = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            prio = atoi(argv[++i]);
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            pid = atoi(argv[++i]);
        else if (argv[i][0] != '-')
            break;
    }

    kprintf("ionice: pid %d -> class %s prio %d (stub)\n",
            pid, class_name(cls), prio);
    return 0;
}

void ionice_init(void)
{
    kprintf("[OK] cmd_ionice: I/O priority command ready\n");
}
