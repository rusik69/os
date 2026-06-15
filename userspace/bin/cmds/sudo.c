/* sudo.c — Execute command as superuser */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: sudo <command [args...]>\n");
        return 1;
    }

    int cur_uid = getuid();
    if (cur_uid != 0) {
        printf("sudo: must be root to use sudo\n");
        return 1;
    }

    /* Construct argv for the command (skip argv[0] which is "sudo") */
    char *cmd_argv[256];
    int ac = 0;
    for (int i = 1; i < argc && ac < 255; i++) {
        cmd_argv[ac++] = argv[i];
    }
    cmd_argv[ac] = NULL;

    int pid = fork();
    if (pid == 0) {
        /* Child: execute the command */
        execve(cmd_argv[0], cmd_argv, (char *const[]){ NULL });
        printf("sudo: cannot execute '%s'\n", cmd_argv[0]);
        exit(1);
    }

    if (pid < 0) {
        printf("sudo: fork failed\n");
        return 1;
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);

    if (status)
        return 1;
    return 0;
}
