/* cmd_free.c — free command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/* Parse a value from /proc/meminfo line like "MemTotal:      123456 kB" */
static unsigned long long parse_meminfo_value(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    unsigned long long val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

/* Human-readable size */
static const char *fmt_size(unsigned long long kb, char *out, int out_sz) {
    if (kb >= 1024) {
        int whole = (int)(kb / 1024);
        int frac = (int)((kb % 1024) * 100 / 1024);
        snprintf(out, out_sz, "%d.%02d MB", whole, frac);
    } else {
        snprintf(out, out_sz, "%llu KB", kb);
    }
    return out;
}

void cmd_free(const char *args) { (void)args;
    char buf[2048];
    uint32_t sz = 0;

    if (vfs_read("/proc/meminfo", buf, sizeof(buf) - 1, &sz) != 0 || sz == 0) {
        /* Fallback to PMM stats if /proc fails */
        struct libc_pmm_stats stats;
        if (libc_pmm_get_stats(&stats) == 0) {
            uint64_t total = stats.total_pages * 4;  /* pages to KB */
            uint64_t free_p = stats.free_pages * 4;
            uint64_t used = total - free_p;
            kprintf("              total      used       free      shared  buffers   cached\n");
            kprintf("Mem:     %9llu %9llu  %9llu %8llu %8llu %8llu  KB\n",
                    (unsigned long long)total, (unsigned long long)used,
                    (unsigned long long)free_p, 0ULL, 0ULL, 0ULL);
            return;
        }
        kprintf("free: cannot read memory info\n");
        return;
    }
    buf[sz] = '\0';

    unsigned long long mem_total_kb = parse_meminfo_value(buf, "MemTotal:");
    unsigned long long mem_free_kb = parse_meminfo_value(buf, "MemFree:");
    unsigned long long buffers_kb = parse_meminfo_value(buf, "Buffers:");
    unsigned long long cached_kb = parse_meminfo_value(buf, "Cached:");
    unsigned long long used_kb = mem_total_kb - mem_free_kb;

    char total_fmt[32], used_fmt[32], free_fmt[32], bufs_fmt[32], cach_fmt[32];
    fmt_size(mem_total_kb, total_fmt, sizeof(total_fmt));
    fmt_size(used_kb, used_fmt, sizeof(used_fmt));
    fmt_size(mem_free_kb, free_fmt, sizeof(free_fmt));
    fmt_size(buffers_kb, bufs_fmt, sizeof(bufs_fmt));
    fmt_size(cached_kb, cach_fmt, sizeof(cach_fmt));

    kprintf("%-14s %10s %10s %10s %10s %10s\n",
            "", "total", "used", "free", "buffers", "cached");
    kprintf("%-14s %10s %10s %10s %10s %10s\n",
            "Mem:", total_fmt, used_fmt, free_fmt, bufs_fmt, cach_fmt);

    unsigned long long actual_avail = mem_free_kb + buffers_kb + cached_kb;
    char avail_fmt[32];
    fmt_size(actual_avail, avail_fmt, sizeof(avail_fmt));
    kprintf("%-14s %10s\n", "Avail:", avail_fmt);
}
