/* renice.c — alter priority of a running process via the setpriority syscall. */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#ifndef SYS_SETPRIORITY_PID
#define SYS_SETPRIORITY_PID 219
#endif
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

/* Raw syscall: SYS_SETPRIORITY_PID(which, pid, prio) */
static long sys_setpriority_pid(int which, int pid, int prio) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_SETPRIORITY_PID),
          "D"((long)which),
          "S"((long)pid),
          "d"((long)prio)
        : "rcx", "r11", "memory");
    return ret;
}

int main(int argc, char *argv[]) {
    int nice_val = 0;
    int pid = -1;

    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) {
        i++;
        if (i >= argc) {
            printf("renice: missing priority after -n\n");
            return 1;
        }
        nice_val = atoi(argv[i]);
        i++;
    }

    if (i >= argc) {
        printf("Usage: renice [-n] priority pid\n");
        return 1;
    }

    /* If the first positional arg is numeric (no -n), treat as priority */
    if (nice_val == 0 && i < argc) {
        /* Check if argv[i] looks like a number (priority) */
        const char *p = argv[i];
        if ((*p == '-' || *p == '+') || (*p >= '0' && *p <= '9')) {
            nice_val = atoi(argv[i]);
            i++;
        }
    }

    if (i >= argc) {
        printf("renice: missing pid\n");
        return 1;
    }
    pid = atoi(argv[i]);

    if (pid <= 0) {
        printf("renice: invalid pid: %s\n", argv[i]);
        return 1;
    }

    long rc = sys_setpriority_pid(PRIO_PROCESS, pid, nice_val);
    if (rc < 0) {
        printf("renice: cannot set priority %d for pid %d\n", nice_val, pid);
        return 1;
    }

    printf("priority set to %d\n", nice_val);
    return 0;
}
