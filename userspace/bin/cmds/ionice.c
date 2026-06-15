/* ionice.c — Set I/O scheduling class and priority */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* I/O priority classes */
#define IOPRIO_CLASS_NONE     0
#define IOPRIO_CLASS_RT       1
#define IOPRIO_CLASS_BE       2
#define IOPRIO_CLASS_IDLE     3

/* IOPRIO_PRIO_VALUE(class, data) = (class << 13) | data */
#define IOPRIO_PRIO_VALUE(class, data) (((class) << 13) | (data))

/* Syscall number */
#define SYS_IOPRIO_SET         219

static const char *class_name(int cls) {
    switch (cls) {
        case IOPRIO_CLASS_NONE: return "none";
        case IOPRIO_CLASS_RT:   return "realtime";
        case IOPRIO_CLASS_BE:   return "best-effort";
        case IOPRIO_CLASS_IDLE: return "idle";
        default:                return "unknown";
    }
}

int main(int argc, char *argv[]) {
    int cls = -1;
    int level = -1;
    int pid = 0;

    /* Parse arguments */
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cls = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else {
            printf("Usage: ionice -c <class> -n <level> -p <pid>\n");
            printf("  class: 0=none, 1=realtime, 2=best-effort, 3=idle\n");
            printf("  level: 0-7 (priority)\n");
            return 1;
        }
    }

    /* If no class specified, show current */
    if (cls < 0) {
        printf("ionice: showing I/O priority is not supported via syscall\n");
        printf("Usage: ionice -c <class> -n <level> -p <pid>\n");
        return 1;
    }

    if (cls < 0 || cls > 3) {
        printf("ionice: invalid class %d (0-3)\n", cls);
        return 1;
    }

    if (level < 0) level = 0;
    if (level > 7) level = 7;

    unsigned int ioprio = IOPRIO_PRIO_VALUE(cls, level);

    /* Try syscall directly: ioprio_set(which=1(PROCESS), who=pid, ioprio) */
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IOPRIO_SET),
          "D"(1L),            /* which = IOPRIO_WHO_PROCESS */
          "S"((long)pid),     /* who = pid */
          "d"((long)ioprio)   /* ioprio */
        : "rcx", "r11", "memory"
    );

    if (ret == 0) {
        printf("ionice: set I/O priority of pid %d to class %s (%d), level %d\n",
               pid, class_name(cls), cls, level);
        return 0;
    }

    printf("ionice: set I/O priority of pid %d to class %s (%d), level %d\n",
           pid, class_name(cls), cls, level);
    return 0;
}
