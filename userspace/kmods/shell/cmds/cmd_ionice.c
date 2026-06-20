/* cmd_ionice.c — I/O priority */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

/* I/O priority constants (from ioprio.h) */
#define IOPRIO_CLASS_SHIFT      13
#define IOPRIO_PRIO_MASK        ((1UL << IOPRIO_CLASS_SHIFT) - 1)
#define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | (data))

#define IOPRIO_CLASS_NONE       0
#define IOPRIO_CLASS_RT         1
#define IOPRIO_CLASS_BE         2
#define IOPRIO_CLASS_IDLE       3
#define IOPRIO_BE_DEF_PRIO      4

#define IOPRIO_WHO_PROCESS      1

static const char *class_name(int cls)
{
    switch (cls) {
        case IOPRIO_CLASS_NONE: return "none";
        case IOPRIO_CLASS_RT:   return "realtime";
        case IOPRIO_CLASS_BE:   return "best-effort";
        case IOPRIO_CLASS_IDLE: return "idle";
        default:                return "unknown";
    }
}

int cmd_ionice(int argc, char **argv)
{
    int cls = IOPRIO_CLASS_BE;
    int prio = IOPRIO_BE_DEF_PRIO;
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

    if (cls < IOPRIO_CLASS_NONE || cls > IOPRIO_CLASS_IDLE) {
        kprintf("ionice: invalid class %d (use 0-3)\n", cls);
        return 1;
    }
    if (prio < 0 || prio > 7) {
        kprintf("ionice: invalid priority %d (use 0-7)\n", prio);
        return 1;
    }

    uint16_t ioprio_val = IOPRIO_PRIO_VALUE((unsigned int)cls, (unsigned int)prio);

    /* Use the ioprio_set syscall (SYS_IOPRIO_SET = 555) */
    int ret = (int)libc_syscall(555,
        (uint64_t)IOPRIO_WHO_PROCESS, (uint64_t)(uint32_t)pid,
        (uint64_t)ioprio_val, 0, 0);

    if (ret < 0) {
        kprintf("ionice: failed to set I/O priority for pid %d\n", pid);
        return 1;
    }

    kprintf("ionice: pid %d -> class %s prio %d\n", pid, class_name(cls), prio);
    return 0;
}

void ionice_init(void)
{
    kprintf("[OK] cmd_ionice: I/O priority command ready\n");
}
