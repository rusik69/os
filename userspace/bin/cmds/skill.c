/* skill.c — send signal to processes matching criteria (stub) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: skill [signal] PROCESS...\n");
        return 1;
    }
    printf("skill: not fully implemented\n");
    /* Try to send kill anyway */
    int sig = SIGTERM;
    int start = 1;
    if (argc >= 3 && argv[1][0] == '-') {
        sig = atoi(argv[1] + 1);
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid > 0) {
            kill(pid, sig);
            printf("skill: sent signal %d to PID %d\n", sig, pid);
        }
    }
    return 0;
}
