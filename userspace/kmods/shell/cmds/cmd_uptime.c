/* cmd_uptime.c — uptime command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

void cmd_uptime(const char *args) { (void)args;
    char buf[256];
    uint32_t sz = 0;

    /* Read /proc/uptime */
    unsigned long long total_sec = 0;
    if (vfs_read("/proc/uptime", buf, sizeof(buf) - 1, &sz) == 0 && sz > 0) {
        buf[sz] = '\0';
        /* Format: "12345.67 67890.12" */
        char *dot = strchr(buf, '.');
        if (dot) *dot = '\0';
        const char *p = buf;
        /* Skip leading spaces */
        while (*p == ' ') p++;
        /* Parse integer part */
        total_sec = 0;
        while (*p >= '0' && *p <= '9') {
            total_sec = total_sec * 10 + (*p - '0');
            p++;
        }
    } else {
        /* Fallback to timer ticks */
        uint64_t ticks = libc_uptime_ticks();
        total_sec = ticks / TIMER_FREQ;
    }

    /* Read /proc/loadavg */
    char load_buf[128];
    uint32_t load_sz = 0;
    char load_str[64] = "0.00 0.00 0.00";
    if (vfs_read("/proc/loadavg", load_buf, sizeof(load_buf) - 1, &load_sz) == 0 && load_sz > 0) {
        load_buf[load_sz] = '\0';
        /* loadavg format: "N.NN N.NN N.NN N/NNN N" */
        char *nl = strchr(load_buf, '\n');
        if (nl) *nl = '\0';
        /* Copy first 3 load averages */
        int i = 0;
        for (const char *s = load_buf; *s && i < 63; s++) {
            load_str[i++] = *s;
            if (*s == ' ' && *(s+1) == ' ') continue; /* collapse spaces */
        }
        load_str[i] = '\0';
        /* Truncate at the 3rd space to get just the load averages */
        int spaces = 0;
        for (int j = 0; load_str[j]; j++) {
            if (load_str[j] == ' ') {
                spaces++;
                if (spaces >= 3) {
                    load_str[j] = '\0';
                    break;
                }
            }
        }
    }

    uint64_t days = total_sec / 86400;
    uint64_t hours = (total_sec % 86400) / 3600;
    uint64_t minutes = (total_sec % 3600) / 60;
    uint64_t seconds = total_sec % 60;

    if (days > 0)
        kprintf("Uptime: %llu day%s, %llu:%02llu:%02llu\n",
                (unsigned long long)days, days == 1 ? "" : "s",
                (unsigned long long)hours, (unsigned long long)minutes,
                (unsigned long long)seconds);
    else
        kprintf("Uptime: %llu:%02llu:%02llu\n",
                (unsigned long long)hours, (unsigned long long)minutes,
                (unsigned long long)seconds);

    kprintf("Load average: %s\n", load_str);
}
