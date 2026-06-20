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
#define IOPRIO_PRIO_CLASS(prio) ((prio) >> 13)
#define IOPRIO_PRIO_DATA(prio) ((prio) & 0x1FFF)

/* Syscall numbers */
#define SYS_IOPRIO_SET         219
#define SYS_IOPRIO_GET         218

/* IOPRIO_WHO_PROCESS = 1 */
#define IOPRIO_WHO_PROCESS     1

static const char *class_name(int cls) {
    switch (cls) {
        case IOPRIO_CLASS_NONE: return "none";
        case IOPRIO_CLASS_RT:   return "realtime";
        case IOPRIO_CLASS_BE:   return "best-effort";
        case IOPRIO_CLASS_IDLE: return "idle";
        default:                return "unknown";
    }
}

/* Inline syscall for ioprio_get(which, who) */
static int ioprio_get_call(int which, int who) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IOPRIO_GET),
          "D"((long)which),
          "S"((long)who)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

/* Inline syscall for ioprio_set(which, who, ioprio) */
static int ioprio_set_call(int which, int who, unsigned int ioprio) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_IOPRIO_SET),
          "D"((long)which),
          "S"((long)who),
          "d"((long)ioprio)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
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
        int prio = ioprio_get_call(IOPRIO_WHO_PROCESS, pid);
        if (prio < 0) {
            printf("ionice: cannot query I/O priority (err=%d)\n", prio);
            return 1;
        }
        int cur_cls = IOPRIO_PRIO_CLASS(prio);
        int cur_data = IOPRIO_PRIO_DATA(prio);
        printf("ionice: pid %d I/O priority: class %s (%d), level %d\n",
               pid, class_name(cur_cls), cur_cls, cur_data);
        return 0;
    }

    if (cls < 0 || cls > 3) {
        printf("ionice: invalid class %d (0-3)\n", cls);
        return 1;
    }

    if (level < 0) level = 0;
    if (level > 7) level = 7;

    unsigned int ioprio = IOPRIO_PRIO_VALUE(cls, level);

    int ret = ioprio_set_call(IOPRIO_WHO_PROCESS, pid, ioprio);

    if (ret == 0) {
        printf("ionice: set I/O priority of pid %d to class %s (%d), level %d\n",
               pid, class_name(cls), cls, level);
        return 0;
    }

    printf("ionice: set I/O priority of pid %d to class %s (%d), level %d\n",
           pid, class_name(cls), cls, level);
    return 0;
}
