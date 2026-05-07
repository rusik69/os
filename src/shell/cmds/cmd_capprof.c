#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || !*s || !out) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

void cmd_capprof(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: capprof <pid> <none|default|trusted>\n");
        return;
    }

    if (!session_is_root()) {
        kprintf("capprof: permission denied (root only)\n");
        return;
    }

    char pid_str[16];
    char profile_str[16];
    int pi = 0;
    int si = 0;
    const char *p = args;

    while (*p == ' ') p++;
    while (*p && *p != ' ' && pi < (int)sizeof(pid_str) - 1) pid_str[pi++] = *p++;
    pid_str[pi] = '\0';
    while (*p == ' ') p++;
    while (*p && *p != ' ' && si < (int)sizeof(profile_str) - 1) profile_str[si++] = *p++;
    profile_str[si] = '\0';

    if (pid_str[0] == '\0' || profile_str[0] == '\0') {
        kprintf("Usage: capprof <pid> <none|default|trusted>\n");
        return;
    }

    uint32_t pid = 0;
    if (parse_u32(pid_str, &pid) < 0) {
        kprintf("capprof: invalid pid '%s'\n", pid_str);
        return;
    }

    uint32_t profile = 0;
    if (strcmp(profile_str, "none") == 0) {
        profile = LIBC_PROC_CAP_NONE;
    } else if (strcmp(profile_str, "default") == 0) {
        profile = LIBC_PROC_CAP_DEFAULT;
    } else if (strcmp(profile_str, "trusted") == 0) {
        profile = LIBC_PROC_CAP_TRUSTED;
    } else {
        kprintf("capprof: invalid profile '%s' (use none|default|trusted)\n", profile_str);
        return;
    }

    int rc = libc_process_set_cap_profile(pid, profile);
    if (rc == 0) {
        kprintf("capprof: pid %u -> %s\n", (uint64_t)pid, profile_str);
    } else if (rc == -1) {
        kprintf("capprof: permission denied\n");
    } else if (rc == -2) {
        kprintf("capprof: pid %u not found\n", (uint64_t)pid);
    } else if (rc == -3) {
        kprintf("capprof: pid %u is not a user process\n", (uint64_t)pid);
    } else {
        kprintf("capprof: failed (%d)\n", (uint64_t)(-rc));
    }
}
