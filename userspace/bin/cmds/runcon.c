/* runcon.c — run command with security context */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: runcon <context> <command [args...]>\n");
        printf("Run a command with a specified security context.\n");
        return 1;
    }

    const char *context = argv[1];

    /* Try to set security context via /proc/self/attr/current */
    int fd = open("/proc/self/attr/current", O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, context, strlen(context));
        close(fd);
    } else {
        /* Also try /proc/attr/current */
        fd = open("/proc/attr/current", O_WRONLY, 0);
        if (fd >= 0) {
            write(fd, context, strlen(context));
            close(fd);
        } else {
            printf("runcon: cannot set security context '%s' (SMACK/LSM not available?)\n", context);
            printf("runcon: continuing without context change\n");
        }
    }

    /* Build argv for child: argv[2..] become argv[0..] */
    int pid = fork();
    if (pid < 0) {
        printf("runcon: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: exec the command */
        char *dummy_env[] = {NULL};
        execve(argv[2], argv + 2, dummy_env);
        printf("runcon: exec '%s' failed\n", argv[2]);
        return 1;
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    if (status != 0)
        return status;
    return 0;
}
