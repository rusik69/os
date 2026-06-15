/* cpufreq_info.c — Show CPU frequency info */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int found = 0;
    char path[128];
    char buf[256];
    char cpu_present[64];
    int max_cpu = 0;

    /* Try /sys/devices/system/cpu/present to get CPU range */
    int fd = open("/sys/devices/system/cpu/present", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, cpu_present, sizeof(cpu_present) - 1);
        close(fd);
        if (n > 0) {
            cpu_present[n] = '\0';
            while (n > 0 && (cpu_present[n-1] == '\n' || cpu_present[n-1] == ' ')) cpu_present[--n] = '\0';
            printf("CPUs present: %s\n", cpu_present);
            /* Parse range like "0-3" */
            char *dash = strchr(cpu_present, '-');
            if (dash) {
                max_cpu = atoi(dash + 1);
            } else {
                max_cpu = atoi(cpu_present);
            }
        }
    } else {
        /* Assume at least CPU0 */
        max_cpu = 0;
    }

    for (int cpu = 0; cpu <= max_cpu; cpu++) {
        found = 1;
        printf("CPU %d:\n", cpu);

        /* Try cpufreq files */
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        fd = open(path, O_RDONLY, 0);
        if (fd >= 0) {
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
                printf("  Current frequency: %s kHz\n", buf);
            }
        } else {
            /* Try cpuinfo_cur_freq as fallback */
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", cpu);
            fd = open(path, O_RDONLY, 0);
            if (fd >= 0) {
                int n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0) {
                    buf[n] = '\0';
                    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
                    printf("  Current frequency: %s kHz\n", buf);
                }
            } else {
                printf("  Current frequency: (cpufreq not available)\n");
            }
        }

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
        fd = open(path, O_RDONLY, 0);
        if (fd >= 0) {
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
                printf("  Min frequency:     %s kHz\n", buf);
            }
        }

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
        fd = open(path, O_RDONLY, 0);
        if (fd >= 0) {
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
                printf("  Max frequency:     %s kHz\n", buf);
            }
        }

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        fd = open(path, O_RDONLY, 0);
        if (fd >= 0) {
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
                printf("  Governor:         %s\n", buf);
            }
        }
    }

    if (!found) {
        printf("cpufreq_info: /sys/devices/system/cpu not available\n");
        return 1;
    }

    return 0;
}
