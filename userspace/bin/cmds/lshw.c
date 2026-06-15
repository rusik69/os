/* lshw.c — list hardware: read /proc/cpuinfo, /proc/meminfo, /proc/uptime */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Find a value by key in text like /proc/cpuinfo or /proc/meminfo */
static const char *find_value(const char *text, const char *key) {
    const char *p = text;
    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);

        unsigned long klen = strlen(key);
        if ((unsigned long)(line_end - p) > klen && memcmp(p, key, klen) == 0) {
            const char *sep = p + klen;
            if (*sep == ':' || *sep == ' ') {
                const char *val = sep + 1;
                while (*val == ' ' || *val == '\t') val++;
                return val;
            }
        }

        p = *line_end ? line_end + 1 : NULL;
    }
    return NULL;
}

static void print_file_section(const char *path, const char *label) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("  (cannot access %s)\n", path);
        return;
    }
    printf("\n--- %s ---\n", label);
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

int main(void){
    char cpuinfo[4096] = "";
    char meminfo[4096] = "";
    char uptime_str[256] = "";
    char loadavg_str[256] = "";

    int fd = open("/proc/cpuinfo", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, cpuinfo, sizeof(cpuinfo) - 1);
        close(fd);
        if (n > 0) cpuinfo[n] = '\0';
    }

    fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, meminfo, sizeof(meminfo) - 1);
        close(fd);
        if (n > 0) meminfo[n] = '\0';
    }

    fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, uptime_str, sizeof(uptime_str) - 1);
        close(fd);
        if (n > 0) {
            uptime_str[n] = '\0';
            while (n > 0 && (uptime_str[n-1] == '\n' || uptime_str[n-1] == ' ')) uptime_str[--n] = '\0';
        }
    }

    fd = open("/proc/loadavg", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, loadavg_str, sizeof(loadavg_str) - 1);
        close(fd);
        if (n > 0) {
            loadavg_str[n] = '\0';
            while (n > 0 && (loadavg_str[n-1] == '\n' || loadavg_str[n-1] == ' ')) loadavg_str[--n] = '\0';
        }
    }

    printf("Hardware Information:\n");
    printf("====================\n");

    /* System info */
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("System:        %s %s %s\n", uts.sysname, uts.release, uts.version);
        printf("Machine:       %s\n", uts.machine);
        printf("Hostname:      %s\n", uts.nodename);
    }

    /* Uptime */
    if (uptime_str[0]) {
        double uptime_secs = 0;
        const char *p = uptime_str;
        uptime_secs = 0;
        while (*p >= '0' && *p <= '9') {
            uptime_secs = uptime_secs * 10 + (*p - '0');
            p++;
        }
        if (*p == '.') {
            p++;
            double frac = 0;
            double div = 1;
            while (*p >= '0' && *p <= '9') {
                frac = frac * 10 + (*p - '0');
                div *= 10;
                p++;
            }
            uptime_secs += frac / div;
        }
        unsigned long days = (unsigned long)uptime_secs / 86400;
        unsigned long hours = ((unsigned long)uptime_secs % 86400) / 3600;
        unsigned long mins = ((unsigned long)uptime_secs % 3600) / 60;
        printf("Uptime:        %lu days, %lu hours, %lu minutes\n", days, hours, mins);
    }

    /* Load average */
    if (loadavg_str[0]) {
        printf("Load average:  %s\n", loadavg_str);
    }

    /* CPU info */
    {
        const char *val;
        int cpu_count = 0;

        val = find_value(cpuinfo, "processor");
        if (val) {
            const char *p = cpuinfo;
            while ((p = strstr(p, "processor")) != NULL) {
                cpu_count++;
                p++;
            }
        }

        val = find_value(cpuinfo, "model name");
        if (val) {
            char model[128] = "";
            const char *end = strchr(val, '\n');
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(model) - 1) len = sizeof(model) - 1;
            memcpy(model, val, len);
            model[len] = '\0';
            printf("CPU:           %d x %s\n", cpu_count > 0 ? cpu_count : 1, model);
        } else {
            printf("CPU:           %d processor(s)\n", cpu_count > 0 ? cpu_count : 1);
        }

        val = find_value(cpuinfo, "cpu MHz");
        if (val) {
            char mhz[32] = "";
            const char *end = strchr(val, '\n');
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(mhz) - 1) len = sizeof(mhz) - 1;
            memcpy(mhz, val, len);
            mhz[len] = '\0';
            printf("CPU MHz:       %s\n", mhz);
        }

        val = find_value(cpuinfo, "cache size");
        if (val) {
            char cache[32] = "";
            const char *end = strchr(val, '\n');
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(cache) - 1) len = sizeof(cache) - 1;
            memcpy(cache, val, len);
            cache[len] = '\0';
            printf("L2 cache:      %s\n", cache);
        }
    }

    /* Memory info */
    {
        const char *val;
        unsigned long long mem_total = 0, mem_free = 0, mem_avail = 0;
        unsigned long long swap_total = 0, swap_free = 0;

        val = find_value(meminfo, "MemTotal");
        if (val) {
            const char *end = strchr(val, ' ');
            char total_str[32];
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(total_str) - 1) len = sizeof(total_str) - 1;
            memcpy(total_str, val, len);
            total_str[len] = '\0';
            mem_total = 0;
            const char *p = total_str;
            while (*p >= '0' && *p <= '9') {
                mem_total = mem_total * 10 + (*p - '0');
                p++;
            }
        }

        val = find_value(meminfo, "MemFree");
        if (val) {
            const char *end = strchr(val, ' ');
            char free_str[32];
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(free_str) - 1) len = sizeof(free_str) - 1;
            memcpy(free_str, val, len);
            free_str[len] = '\0';
            mem_free = 0;
            const char *p = free_str;
            while (*p >= '0' && *p <= '9') {
                mem_free = mem_free * 10 + (*p - '0');
                p++;
            }
        }

        val = find_value(meminfo, "MemAvailable");
        if (val) {
            const char *end = strchr(val, ' ');
            char avail_str[32];
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(avail_str) - 1) len = sizeof(avail_str) - 1;
            memcpy(avail_str, val, len);
            avail_str[len] = '\0';
            mem_avail = 0;
            const char *p = avail_str;
            while (*p >= '0' && *p <= '9') {
                mem_avail = mem_avail * 10 + (*p - '0');
                p++;
            }
        }

        val = find_value(meminfo, "SwapTotal");
        if (val) {
            const char *end = strchr(val, ' ');
            char total_str[32];
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(total_str) - 1) len = sizeof(total_str) - 1;
            memcpy(total_str, val, len);
            total_str[len] = '\0';
            swap_total = 0;
            const char *p = total_str;
            while (*p >= '0' && *p <= '9') {
                swap_total = swap_total * 10 + (*p - '0');
                p++;
            }
        }

        val = find_value(meminfo, "SwapFree");
        if (val) {
            const char *end = strchr(val, ' ');
            char free_str[32];
            unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
            if (len > sizeof(free_str) - 1) len = sizeof(free_str) - 1;
            memcpy(free_str, val, len);
            free_str[len] = '\0';
            swap_free = 0;
            const char *p = free_str;
            while (*p >= '0' && *p <= '9') {
                swap_free = swap_free * 10 + (*p - '0');
                p++;
            }
        }

        if (mem_total > 0) {
            unsigned long long used = mem_total - mem_free;
            printf("Memory:        %llu KB total, %llu KB used, %llu KB free",
                   mem_total, used, mem_free);
            if (mem_avail > 0) printf(", %llu KB available", mem_avail);
            printf("\n");
        }
        if (swap_total > 0) {
            unsigned long long swap_used = swap_total - swap_free;
            printf("Swap:          %llu KB total, %llu KB used, %llu KB free\n",
                   swap_total, swap_used, swap_free);
        }
    }

    /* Show raw sections */
    print_file_section("/proc/cpuinfo", "/proc/cpuinfo");
    print_file_section("/proc/meminfo", "/proc/meminfo");

    return 0;
}
