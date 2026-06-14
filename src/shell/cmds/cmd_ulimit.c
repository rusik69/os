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

#define NUM_RLIM 15

static void ulimit_show_all(int hard) {
    for (int i = 0; i < NUM_RLIM; i++) {
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
        else
            kprintf("%s\t%s\t%llu%s\n", hard ? "-H" : "-S", rlim_names[i],
                    (unsigned long long)val, unit);
    }
}

static void ulimit_show_one(int resource, int hard) {
    if (resource < 0 || resource >= NUM_RLIM) {
        kprintf("ulimit: invalid resource %d (must be 0-%d)\n", resource, NUM_RLIM - 1);
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

/* Validate that a flag is followed by valid content (not another flag) */
static int validate_flag(const char **p, int *hard, int *show_all) {
    if (**p == '-') {
        (*p)++;
        if (**p == 'H') { *hard = 1; (*p)++; }
        else if (**p == 'S') { *hard = 0; (*p)++; }
        else if (**p == 'a') { *show_all = 1; (*p)++; }
        else {
            kprintf("ulimit: invalid option '-%c' (use -S, -H, or -a)\n", **p);
            return -1;
        }
        return 0;
    }
    return -1; /* not a flag */
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
            /* Validate flag */
            int ret = validate_flag(&p, &hard, &show_all);
            if (ret < 0) return;

            /* Parse resource identifier (numeric) */
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                resource = 0;
                while (*p >= '0' && *p <= '9') {
                    resource = resource * 10 + (*p - '0');
                    p++;
                }
                /* Validate resource range */
                if (resource < 0 || resource >= NUM_RLIM) {
                    kprintf("ulimit: invalid resource %d (must be 0-%d)\n", resource, NUM_RLIM - 1);
                    return;
                }
            }
        } else if (*p >= '0' && *p <= '9') {
            /* Parse: resource [value] */
            resource = 0;
            while (*p >= '0' && *p <= '9') {
                resource = resource * 10 + (*p - '0');
                p++;
            }
            /* Validate resource range */
            if (resource < 0 || resource >= NUM_RLIM) {
                kprintf("ulimit: invalid resource %d (must be 0-%d)\n", resource, NUM_RLIM - 1);
                return;
            }
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                /* Parse value, validate no overflow */
                set_val = 0;
                uint64_t prev = 0;
                while (*p >= '0' && *p <= '9') {
                    prev = set_val;
                    set_val = set_val * 10 + (*p - '0');
                    /* Check for overflow */
                    if (set_val / 10 != prev && prev > 0) {
                        kprintf("ulimit: value overflow (too large)\n");
                        return;
                    }
                    p++;
                }
                if (set_val == 0) {
                    /* Zero is valid for some limits but make sure it's not just missing */
                    kprintf("ulimit: value must be positive\n");
                    return;
                }
                do_set = 1;
            } else if (*p) {
                kprintf("ulimit: unexpected trailing characters '%s'\n", p);
                return;
            }
        }
    }

    if (show_all) {
        ulimit_show_all(hard);
        return;
    }

    if (resource >= 0 && do_set) {
        /* Validate: cannot set negative values (already positive checked) */
        if (set_val == RLIM_INFINITY) {
            kprintf("ulimit: cannot set unlimited using this interface\n");
            return;
        }

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
