/* skill.c — send signal to processes matching criteria */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_PIDS 4096
#define CMDLINE_BUF 256

/* Read process name from /proc/<pid>/comm (or fallback to stat) */
static int read_proc_name(int pid, char *name, int maxlen) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    int fd = open(path, 0, 0);
    if (fd >= 0) {
        int n = read(fd, name, maxlen - 1);
        close(fd);
        if (n > 0) {
            name[n] = '\0';
            /* Strip trailing newline */
            while (n > 0 && (name[n-1] == '\n' || name[n-1] == '\r'))
                name[--n] = '\0';
            return 1;
        }
    }
    /* Fallback: parse /proc/<pid>/stat for process name in parentheses */
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fd = open(path, 0, 0);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            /* Find name in parentheses: e.g., "123 (process_name) ..." */
            char *lp = strchr(buf, '(');
            char *rp = strrchr(buf, ')');
            if (lp && rp && rp > lp) {
                int len = rp - lp - 1;
                if (len > maxlen - 1) len = maxlen - 1;
                memcpy(name, lp + 1, len);
                name[len] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static int find_pids_by_name(const char *name, int *pids, int max) {
    int count = 0;

    /* Iterate /proc directory for numeric entries (PIDs) */
    int fd = open("/proc", O_RDONLY, 0);
    if (fd < 0) return 0;

    char dent_buf[4096];
    int n = getdents64(fd, dent_buf, sizeof(dent_buf));
    close(fd);

    if (n <= 0) return 0;

    int pos = 0;
    while (pos < n && count < max) {
        struct dirent *de = (struct dirent *)(dent_buf + pos);
        if (de->d_name[0] >= '0' && de->d_name[0] <= '9') {
            int pid = 0;
            for (char *p = de->d_name; *p >= '0' && *p <= '9'; p++)
                pid = pid * 10 + (*p - '0');

            char pname[CMDLINE_BUF];
            if (read_proc_name(pid, pname, sizeof(pname))) {
                /* Match name (case-insensitive substring or exact) */
                if (strstr(pname, name) || strstr(name, pname)) {
                    pids[count++] = pid;
                }
            }
        }
        if (de->d_reclen == 0) break;
        pos += de->d_reclen;
    }

    return count;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: skill [signal] PROCESS...\n");
        return 1;
    }

    int sig = SIGTERM;
    int start = 1;
    if (argc >= 3 && argv[1][0] == '-') {
        sig = atoi(argv[1] + 1);
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid > 0) {
            /* Numeric argument: treat as PID */
            kill(pid, sig);
            printf("skill: sent signal %d to PID %d\n", sig, pid);
        } else {
            /* Non-numeric argument: treat as process name */
            int pids[MAX_PIDS];
            int count = find_pids_by_name(argv[i], pids, MAX_PIDS);
            if (count == 0) {
                printf("skill: no process matching '%s'\n", argv[i]);
            } else {
                for (int j = 0; j < count; j++) {
                    kill(pids[j], sig);
                    printf("skill: sent signal %d to PID %d (%s)\n", sig, pids[j], argv[i]);
                }
            }
        }
    }
    return 0;
}
