/* timeout.c — run command with time limit */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static volatile int timed_out = 0;

static void alarm_handler(int sig) {
    (void)sig;
    timed_out = 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: timeout DURATION COMMAND [ARG...]\n");
        return 1;
    }
    unsigned int duration = 0;
    char *d = argv[1];
    while (*d >= '0' && *d <= '9') {
        duration = duration * 10 + (*d - '0');
        d++;
    }
    if (duration == 0) {
        printf("timeout: invalid duration\n");
        return 1;
    }
    int pid = fork();
    if (pid < 0) {
        printf("timeout: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        /* Child: exec the command */
        execve(argv[2], argv + 2, NULL);
        printf("timeout: cannot exec '%s'\n", argv[2]);
        exit(1);
    }
    /* Parent: set alarm and wait */
    signal(SIGALRM, alarm_handler);
    alarm(duration);
    int status;
    waitpid(pid, &status, 0);
    if (timed_out) {
        /* Kill the child if still running */
        kill(pid, SIGKILL);
        printf("timeout: command timed out after %u seconds\n", duration);
        return 124;
    }
    if (status < 0) return 1;
    return status;
}
