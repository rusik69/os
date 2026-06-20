/* ulimit.c — get/set resource limits */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Syscall numbers not in libc header */
#define SYS_PRLIMIT64 272
#define SYS_GETRLIMIT 127
#define SYS_SETRLIMIT 128
#define RLIM_INFINITY (~0ULL)

/* Resource limit struct matching kernel's rlimit64 */
struct rlimit64 {
    unsigned long long cur;
    unsigned long long max;
};

/* Old-style rlimit (32-bit fields) for getrlimit/setrlimit fallback */
struct rlimit {
    unsigned long cur;
    unsigned long max;
};

/* Resource names */
static const char *rlim_names[] = {
    "AS (address space)",
    "CORE (core file size)",
    "CPU (CPU time)",
    "DATA (data segment)",
    "FSIZE (file size)",
    "NOFILE (open files)",
    "STACK (stack size)",
    "NPROC (process count)",
    "MEMLOCK (locked memory)",
    "LOCKS (file locks)",
    "SIGPENDING (pending signals)",
    "MSGQUEUE (message queue size)",
    "NICE (nice value)",
    "RTPRIO (realtime priority)",
    "RSS (resident set size)",
};

#define NUM_RLIM 15

/* Raw syscall wrapper for prlimit64(pid, resource, new_limit, old_limit) */
static long prlimit64_call(int pid, int resource,
                           const struct rlimit64 *newp,
                           struct rlimit64 *oldp)
{
    long ret;
    __asm__ volatile (
        "movq %5, %%r10\n"
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_PRLIMIT64),
          "D"((long)pid),
          "S"((long)resource),
          "d"((long)newp),
          "r"((long)oldp)
        : "rcx", "r11", "r10", "memory"
    );
    return ret;
}

/* Fallback: getrlimit syscall (old interface with 32-bit fields) */
static long getrlimit_call(int resource, struct rlimit *rlim) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_GETRLIMIT),
          "D"((long)resource),
          "S"((long)rlim)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Fallback: setrlimit syscall */
static long setrlimit_call(int resource, const struct rlimit *rlim) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_SETRLIMIT),
          "D"((long)resource),
          "S"((long)rlim)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Get a resource limit, trying prlimit64 first, then getrlimit */
static int get_rlimit(int resource, struct rlimit64 *rlim) {
    if (prlimit64_call(0, resource, NULL, rlim) == 0)
        return 0;
    /* Fallback to getrlimit */
    struct rlimit old;
    if (getrlimit_call(resource, &old) == 0) {
        rlim->cur = old.cur;
        rlim->max = old.max;
        return 0;
    }
    return -1;
}

/* Set a resource limit, trying prlimit64 first, then setrlimit */
static int set_rlimit(int resource, const struct rlimit64 *rlim) {
    struct rlimit64 old;
    if (prlimit64_call(0, resource, rlim, &old) == 0)
        return 0;
    /* Fallback to setrlimit (old struct may truncate large values) */
    struct rlimit nr;
    nr.cur = (unsigned long)rlim->cur;
    nr.max = (unsigned long)rlim->max;
    return setrlimit_call(resource, &nr);
}

static const char *unit_str(int i) {
    if (i == 0 || i == 1 || i == 3 || i == 4 || i == 6 || i == 8 || i == 11)
        return " bytes";
    if (i == 2)
        return " seconds";
    return "";
}

static void show_all(void) {
    int shown = 0;
    for (int i = 0; i < NUM_RLIM; i++) {
        struct rlimit64 rlim;
        if (get_rlimit(i, &rlim) != 0)
            continue;
        shown = 1;
        const char *unit = unit_str(i);
        if (rlim.cur == RLIM_INFINITY)
            printf("%s\tunlimited\n", rlim_names[i]);
        else
            printf("%s\t%llu%s\n", rlim_names[i],
                   (unsigned long long)rlim.cur, unit);
    }
    if (!shown)
        printf("ulimit: no resource limits available\n");
}

static void show_one(int resource) {
    if (resource < 0 || resource >= NUM_RLIM) {
        printf("ulimit: invalid resource %d\n", resource);
        return;
    }
    struct rlimit64 rlim;
    if (get_rlimit(resource, &rlim) != 0) {
        printf("ulimit: getrlimit failed\n");
        return;
    }
    if (rlim.cur == RLIM_INFINITY)
        printf("unlimited\n");
    else
        printf("%llu%s\n", (unsigned long long)rlim.cur, unit_str(resource));
}

static int set_one(int resource, unsigned long long val) {
    if (resource < 0 || resource >= NUM_RLIM) {
        printf("ulimit: invalid resource %d\n", resource);
        return 1;
    }
    struct rlimit64 old, new;
    if (get_rlimit(resource, &old) != 0) {
        printf("ulimit: get failed\n");
        return 1;
    }
    new.cur = val;
    new.max = old.max;
    if (set_rlimit(resource, &new) != 0) {
        printf("ulimit: set failed\n");
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        show_all();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "-a") == 0) {
        show_all();
        return 0;
    }

    if (argc == 2) {
        int res = atoi(argv[1]);
        if (res == 0 && argv[1][0] != '0') {
            printf("ulimit: invalid resource '%s'\n", argv[1]);
            return 1;
        }
        show_one(res);
        return 0;
    }

    if (argc == 3) {
        int res = atoi(argv[1]);
        if (res == 0 && argv[1][0] != '0') {
            printf("ulimit: invalid resource '%s'\n", argv[1]);
            return 1;
        }
        unsigned long long val = 0;
        const char *p = argv[2];
        while (*p) {
            if (*p < '0' || *p > '9') {
                printf("ulimit: invalid value '%s'\n", argv[2]);
                return 1;
            }
            val = val * 10 + (unsigned long long)(*p - '0');
            p++;
        }
        return set_one(res, val);
    }

    printf("usage: ulimit         (show all soft limits)\n");
    printf("       ulimit -a      (show all soft limits)\n");
    printf("       ulimit <N>     (show limit for resource N)\n");
    printf("       ulimit <N> <V> (set soft limit for resource N to V)\n");
    printf("Resources: 0=AS, 1=CORE, 2=CPU, 3=DATA, 4=FSIZE,\n");
    printf("           5=NOFILE, 6=STACK, 7=NPROC, 8=MEMLOCK,\n");
    printf("           9=LOCKS, 10=SIGPENDING, 11=MSGQUEUE,\n");
    printf("           12=NICE, 13=RTPRIO, 14=RSS\n");
    return 1;
}
