/* wait.c — wait for process completion */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: wait [PID...]\n");
        printf("Wait for each specified PID to exit.\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            printf("wait: invalid PID '%s'\n", argv[i]);
            continue;
        }
        int status;
        int ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            printf("wait: waitpid(%d) failed\n", pid);
        } else {
            printf("pid %d exited with status %d\n", pid, status);
        }
    }
    return 0;
}
