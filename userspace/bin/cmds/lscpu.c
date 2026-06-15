/* lscpu.c — CPU architecture info: read /proc/cpuinfo */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Find a value by key in /proc/cpuinfo output */
static const char *find_cpuinfo_value(const char *info, const char *key) {
    const char *p = info;
    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);

        /* Check if this line starts with the key */
        unsigned long klen = strlen(key);
        if ((unsigned long)(line_end - p) > klen && memcmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *val = p + klen + 1;
            while (*val == ' ' || *val == '\t') val++;
            return val;
        }

        p = *line_end ? line_end + 1 : NULL;
    }
    return NULL;
}

int main(void){
    /* Try to read /proc/cpuinfo */
    char cpuinfo[4096] = "";
    char model_name[128] = "Unknown";
    char cpu_mhz[32] = "Unknown";
    char cache_size[32] = "Unknown";
    char vendor_id[32] = "Unknown";
    int cpu_count = 0;

    int fd = open("/proc/cpuinfo", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, cpuinfo, sizeof(cpuinfo) - 1);
        close(fd);
        if (n > 0) {
            cpuinfo[n] = '\0';
            const char *val;

            val = find_cpuinfo_value(cpuinfo, "model name");
            if (val) {
                const char *end = strchr(val, '\n');
                unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
                if (len > sizeof(model_name) - 1) len = sizeof(model_name) - 1;
                memcpy(model_name, val, len);
                model_name[len] = '\0';
            }

            val = find_cpuinfo_value(cpuinfo, "cpu MHz");
            if (val) {
                const char *end = strchr(val, '\n');
                unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
                if (len > sizeof(cpu_mhz) - 1) len = sizeof(cpu_mhz) - 1;
                memcpy(cpu_mhz, val, len);
                cpu_mhz[len] = '\0';
            }

            val = find_cpuinfo_value(cpuinfo, "cache size");
            if (val) {
                const char *end = strchr(val, '\n');
                unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
                if (len > sizeof(cache_size) - 1) len = sizeof(cache_size) - 1;
                memcpy(cache_size, val, len);
                cache_size[len] = '\0';
            }

            val = find_cpuinfo_value(cpuinfo, "vendor_id");
            if (val) {
                const char *end = strchr(val, '\n');
                unsigned long len = end ? (unsigned long)(end - val) : strlen(val);
                if (len > sizeof(vendor_id) - 1) len = sizeof(vendor_id) - 1;
                memcpy(vendor_id, val, len);
                vendor_id[len] = '\0';
            }

            /* Count processors */
            const char *proc = cpuinfo;
            while ((proc = strstr(proc, "processor")) != NULL) {
                cpu_count++;
                proc++;
            }
        }
    }

    struct utsname uts;
    int have_uname = (uname(&uts) == 0);

    printf("Architecture:        ");
    if (have_uname) printf("%s\n", uts.machine);
    else printf("x86_64\n");

    printf("CPU op-mode(s):      32-bit, 64-bit\n");
    printf("Byte Order:          Little Endian\n");
    printf("CPU(s):              %d\n", cpu_count > 0 ? cpu_count : 1);
    printf("Vendor ID:           %s\n", vendor_id);
    printf("Model name:          %s\n", model_name);
    printf("CPU MHz:             %s\n", cpu_mhz);
    printf("L2 cache:            %s\n", cache_size);

    if (have_uname) {
        printf("System:              %s %s\n", uts.sysname, uts.release);
        printf("Hostname:            %s\n", uts.nodename);
    }

    return 0;
}
