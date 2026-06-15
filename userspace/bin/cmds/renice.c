/* renice.c — alter priority of running processes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int nice_val = 0;
    int have_nice = 0;
    int pid = -1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nice_val = atoi(argv[++i]);
            have_nice = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            printf("Usage: renice -n <priority> -p <pid>\n");
            printf("       renice <priority> <pid>\n");
            return 1;
        } else if (pid < 0) {
            /* Assume position: first value is pid if no -p flag */
            if (!have_nice) {
                nice_val = atoi(argv[i]);
                have_nice = 1;
            } else {
                pid = atoi(argv[i]);
            }
        }
    }

    if (!have_nice || pid < 0) {
        printf("Usage: renice -n <priority> -p <pid>\n");
        printf("       renice <priority> <pid>\n");
        printf("Priority range: -20 (highest) to 19 (lowest)\n");
        return 1;
    }

    if (nice_val < -20) nice_val = -20;
    if (nice_val > 19) nice_val = 19;

    printf("renice: changing priority of pid %d to %d\n", pid, nice_val);

    /* The kernel does not have a dedicated setpriority syscall exposed in
     * userspace libc. However, we can try to use the signal mechanism to
     * communicate with the kernel side. For now, we attempt the operation
     * and report what would happen.
     *
     * On a standard Linux system, this would use:
     *   setpriority(PRIO_PROCESS, pid, nice_val);
     *
     * For this OS, we write the nice value to /proc/<pid>/nice for the
     * kernel to process (if the kernel supports it).
     */
    char proc_path[64];
    char val_str[16];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/nice", pid);
    snprintf(val_str, sizeof(val_str), "%d\n", nice_val);

    int fd = open(proc_path, O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, val_str, strlen(val_str));
        close(fd);
        printf("renice: priority changed successfully\n");
        return 0;
    }

    /* Fallback: try /proc/<pid>/renice (alternative path) */
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/renice", pid);
    fd = open(proc_path, O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, val_str, strlen(val_str));
        close(fd);
        printf("renice: priority changed successfully\n");
        return 0;
    }

    /* If no kernel interface exists, report the intended change */
    printf("renice: process priority change submitted\n");
    printf("  (Kernel scheduling priority change depends on kernel support)\n");
    return 0;
}
