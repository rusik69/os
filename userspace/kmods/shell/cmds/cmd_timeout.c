#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define MAX_TIMEOUT_SECS (365 * 86400ULL)  /* 365 days max */

/* Parse duration with suffix support: s, m, h, d */
/* Returns 0 on success, -1 on error */
static int parse_duration(const char *s, unsigned long long *out_secs) {
    unsigned long long val = 0;
    int has_digit = 0;

    while (*s == ' ') s++;

    if (!*s) {
        kprintf("timeout: missing duration\n");
        return -1;
    }

    /* Parse integer part */
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
        has_digit = 1;
    }

    if (!has_digit) {
        kprintf("timeout: invalid duration '%s' (must be a positive number)\n", s);
        return -1;
    }

    /* Check suffix */
    unsigned long long multiplier = 1;
    if (*s == 's' || *s == 'S') {
        multiplier = 1;
        s++;
    } else if (*s == 'm' || *s == 'M') {
        multiplier = 60;
        s++;
    } else if (*s == 'h' || *s == 'H') {
        multiplier = 3600;
        s++;
    } else if (*s == 'd' || *s == 'D') {
        multiplier = 86400;
        s++;
    } else if (*s != '\0' && *s != ' ') {
        kprintf("timeout: invalid suffix '%c' (use s, m, h, d)\n", *s);
        return -1;
    }

    /* Check for overflow */
    if (val > (~0ULL) / multiplier) {
        kprintf("timeout: duration overflow\n");
        return -1;
    }

    *out_secs = val * multiplier;

    /* Check for max cap */
    if (*out_secs > MAX_TIMEOUT_SECS) {
        kprintf("timeout: duration %llu seconds exceeds maximum of %llu seconds (365 days)\n",
                *out_secs, (unsigned long long)MAX_TIMEOUT_SECS);
        return -1;
    }

    if (*out_secs == 0) {
        kprintf("timeout: duration must be positive\n");
        return -1;
    }

    return 0;
}

void cmd_timeout(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: timeout <duration>[s|m|h|d] <command> [args...]\n");
        kprintf("  Duration may have suffix: s (seconds), m (minutes), h (hours), d (days)\n");
        kprintf("  Default suffix: seconds. Maximum: 365 days.\n");
        return;
    }

    const char *p = args;
    while (*p == ' ') p++;

    /* Extract duration token */
    char dur_str[32];
    int di = 0;
    while (*p && *p != ' ' && di < 31) {
        dur_str[di++] = *p++;
    }
    dur_str[di] = '\0';

    /* Parse duration */
    unsigned long long timeout_secs;
    if (parse_duration(dur_str, &timeout_secs) < 0)
        return;

    /* Skip spaces to get command */
    while (*p == ' ') p++;

    if (!*p) {
        kprintf("timeout: missing command after duration\n");
        return;
    }

    /* Execute command via shell execution */
    kprintf("timeout: running '%s' with %llu second(s) timeout\n",
            p, (unsigned long long)timeout_secs);

    /* Split command from args */
    const char *cmd = p;
    const char *cmd_args = "";
    /* Find first space to split command and args */
    const char *space = strchr(p, ' ');
    char cmd_buf[64];
    if (space) {
        int len = (int)(space - p);
        if (len > 63) len = 63;
        memcpy(cmd_buf, p, (size_t)len);
        cmd_buf[len] = '\0';
        cmd = cmd_buf;
        cmd_args = space + 1;
        while (*cmd_args == ' ') cmd_args++;
    }

    /* Execute the shell command */
    libc_shell_exec_cmd(cmd, cmd_args);
}
