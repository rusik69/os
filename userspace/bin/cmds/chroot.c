/* chroot.c — change root directory and run command */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: chroot NEWROOT [COMMAND...]\n");
        return 1;
    }

    const char *newroot = argv[1];

    /* Try chroot syscall first */
    if (chroot(newroot) < 0) {
        /* Fallback: chdir to new root */
        if (chdir(newroot) < 0) {
            printf("chroot: cannot change to '%s'\n", newroot);
            return 1;
        }
    }

    /* Determine command to run (default /bin/sh) */
    const char *cmd;
    char **cmd_argv;
    int cmd_argc;

    if (argc >= 3) {
        cmd = argv[2];
        cmd_argv = argv + 2;
        cmd_argc = argc - 2;
    } else {
        static char *sh_argv[] = {"/bin/sh", NULL};
        cmd = "/bin/sh";
        cmd_argv = sh_argv;
        cmd_argc = 1;
        (void)cmd_argc;
    }

    /* Fork and exec */
    int pid = fork();
    if (pid < 0) {
        printf("chroot: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: exec the command */
        char *envp[] = {NULL};
        execve(cmd, cmd_argv, envp);
        printf("chroot: exec '%s' failed\n", cmd);
        return 1;
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    if (status != 0)
        return status;
    return 0;
}
