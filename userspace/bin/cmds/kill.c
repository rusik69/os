/* kill.c — send signal to a process */

#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: kill [-sig] <pid>\n");
        return 1;
    }
    int sig = 15; /* SIGTERM by default */
    int pid_arg = 1;

    if (argv[1][0] == '-') {
        sig = atoi(argv[1] + 1);
        pid_arg = 2;
        if (sig == 0) sig = 15;
    }

    if (pid_arg >= argc) {
        printf("kill: missing pid\n");
        return 1;
    }

    int pid = atoi(argv[pid_arg]);
    if (pid <= 0) {
        printf("kill: invalid pid: %s\n", argv[pid_arg]);
        return 1;
    }

    if (kill(pid, sig) < 0) {
        printf("kill: failed to send signal %d to pid %d\n", sig, pid);
        return 1;
    }
    return 0;
}
