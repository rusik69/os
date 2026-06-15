/* getty.c — terminal getty */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int local_flag = 0;
    int timeout_secs = 0;
    const char *tty = NULL;
    const char *speed = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0) {
            local_flag = 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            printf("usage: getty [-L] [-t timeout] <tty> [speed]\n");
            printf("  -L          local mode (no carrier detect)\n");
            printf("  -t <sec>    login timeout\n");
            printf("  <tty>       terminal device (e.g. ttyS0)\n");
            printf("  <speed>     baud rate (e.g. 115200)\n");
            return 1;
        } else if (!tty) {
            tty = argv[i];
        } else if (!speed) {
            speed = argv[i];
        }
    }

    (void)local_flag;
    (void)timeout_secs;
    (void)speed;

    if (!tty) {
        printf("usage: getty [-L] [-t timeout] <tty> [speed]\n");
        return 1;
    }

    /* Build full path if not already */
    char tty_path[256];
    if (tty[0] == '/') {
        snprintf(tty_path, sizeof(tty_path), "%s", tty);
    } else {
        snprintf(tty_path, sizeof(tty_path), "/dev/%s", tty);
    }

    /* Open the TTY device */
    int fd = open(tty_path, O_RDWR, 0);
    if (fd < 0) {
        printf("getty: cannot open '%s'\n", tty_path);
        return 1;
    }

    /* Redirect stdin/stdout/stderr to the TTY */
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2) close(fd);

    /* Print /etc/issue if it exists */
    int issue_fd = open("/etc/issue", O_RDONLY, 0);
    if (issue_fd >= 0) {
        char rbuf[1024];
        int n;
        while ((n = read(issue_fd, rbuf, sizeof(rbuf))) > 0) {
            write(STDOUT_FILENO, rbuf, n);
        }
        close(issue_fd);
    }

    /* Print login prompt */
    write(STDOUT_FILENO, "\r\n", 2);

    /* Start shell or login */
    char *shell_args[] = {"/bin/sh", "-i", 0};

    /* Try /bin/login first if it exists */
    if (access("/bin/login", 0) == 0) {
        char *login_args[] = {"/bin/login", 0};
        execve("/bin/login", login_args, 0);
    }

    /* Fallback to shell */
    execve("/bin/sh", shell_args, 0);

    /* If we get here, both failed */
    write(STDOUT_FILENO, "getty: cannot start shell or login\r\n", 36);
    return 1;
}
