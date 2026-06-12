/* cmd_ulimit.c — view/set resource limits */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "syscall.h"

#define RLIM_INFINITY  (~0ULL)

static const char *rlim_names[] = {
    "AS (address space)",
    "CORE (core file size)",
    "CPU (cpu time)",
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

static void ulimit_show_all(int hard) {
    for (int i = 0; i < 15; i++) {
        struct { uint64_t cur, max; } rlim;
        if (libc_syscall(SYS_PRLIMIT64, 0, i, 0, (uint64_t)(uintptr_t)&rlim, 0) != 0)
            continue;
        uint64_t val = hard ? rlim.max : rlim.cur;
        const char *str = val == RLIM_INFINITY ? "unlimited" : "";
        const char *unit = "";
        if (i == 0) unit = " bytes";
        else if (i == 1) unit = " bytes";
        else if (i == 2) unit = " seconds";
        else if (i == 3) unit = " bytes";
        else if (i == 4) unit = " bytes";
        else if (i == 6) unit = " bytes";
        else if (i == 8) unit = " bytes";
        else if (i == 11) unit = " bytes";

        if (val == RLIM_INFINITY)
            kprintf("%s\t%s%s\n", hard ? "-H" : "-S", rlim_names[i], str);
        else if (i == 2)
            kprintf("%s\t%s\t%llu%s\n", hard ? "-H" : "-S", rlim_names[i],
                    (unsigned long long)val, unit);
        else
            kprintf("%s\t%s\t%llu%s\n", hard ? "-H" : "-S", rlim_names[i],
                    (unsigned long long)val, unit);
    }
}

static void ulimit_show_one(int resource, int hard) {
    if (resource < 0 || resource >= 15) {
        kprintf("ulimit: invalid resource %d\n", resource);
        return;
    }
    struct { uint64_t cur, max; } rlim;
    if (libc_syscall(SYS_PRLIMIT64, 0, resource, 0, (uint64_t)(uintptr_t)&rlim, 0) != 0) {
        kprintf("ulimit: prlimit64 failed\n");
        return;
    }
    uint64_t val = hard ? rlim.max : rlim.cur;
    if (val == RLIM_INFINITY)
        kprintf("unlimited\n");
    else
        kprintf("%llu\n", (unsigned long long)val);
}

void cmd_ulimit(const char *args) {
    int hard = 0;
    int show_all = 0;
    int resource = -1;
    uint64_t set_val = 0;
    int do_set = 0;

    /* Very simple argument parsing */
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;

        if (p[0] == '-') {
            p++;
            if (*p == 'H') { hard = 1; p++; }
            else if (*p == 'S') { hard = 0; p++; }
            else if (*p == 'a') { show_all = 1; p++; }

            /* Parse resource identifier */
            if (*p >= '0' && *p <= '9') {
                resource = 0;
                while (*p >= '0' && *p <= '9') {
                    resource = resource * 10 + (*p - '0');
                    p++;
                }
            }
        } else if (*p >= '0' && *p <= '9') {
            /* Just a number: set that resource's soft limit? */
            /* Actually for ulimit, the form 'ulimit resource value' */
            resource = 0;
            while (*p >= '0' && *p <= '9') {
                resource = resource * 10 + (*p - '0');
                p++;
            }
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                set_val = 0;
                while (*p >= '0' && *p <= '9') {
                    set_val = set_val * 10 + (*p - '0');
                    p++;
                }
                do_set = 1;
            }
        }
    }

    if (show_all) {
        ulimit_show_all(hard);
        return;
    }

    if (resource >= 0 && do_set) {
        /* Set the limit */
        struct { uint64_t cur, max; } new_rlim, old_rlim;
        if (libc_syscall(SYS_PRLIMIT64, 0, resource, 0, (uint64_t)(uintptr_t)&old_rlim, 0) != 0) {
            kprintf("ulimit: prlimit64 get failed\n");
            return;
        }
        new_rlim.cur = set_val;
        new_rlim.max = old_rlim.max;
        if (libc_syscall(SYS_PRLIMIT64, 0, resource, (uint64_t)(uintptr_t)&new_rlim,
                         (uint64_t)(uintptr_t)&old_rlim, 0) != 0) {
            kprintf("ulimit: prlimit64 set failed\n");
            return;
        }
        return;
    }

    if (resource >= 0) {
        ulimit_show_one(resource, hard);
        return;
    }

    /* Default: show soft limits */
    ulimit_show_all(0);
}
