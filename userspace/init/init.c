/* Init process — PID 1 for userspace.
 *
 * Opens /dev/console, spawns /bin/sh, then waits for children.
 * This is the first userspace process started by the kernel.
 */

#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[init] PID %d: Starting init process...\n", getpid());

    /* Open /dev/console for stdin/stdout/stderr */
    int console = open("/dev/console", 0);
    if (console < 0) {
        /* Maybe /dev doesn't exist yet — try stdin/stdout directly */
        printf("[init] Warning: no /dev/console, using raw I/O\n");
    } else {
        /* Map the console fd to stdin/stdout/stderr */
        if (console != 0) { dup2(console, 0); close(console); }
        /* Note: stdout and stderr are fds 1 and 2 */
    }

    printf("[init] Starting shell /bin/sh...\n");

    /* Try to spawn /bin/sh */
    while (1) {
        int pid = fork();
        if (pid < 0) {
            printf("[init] fork failed: %d\n", pid);
            break;
        }

        if (pid == 0) {
            /* Child — exec shell */
            char *const argv[] = { "/bin/sh", NULL };
            char *const envp[] = { "PATH=/bin", "HOME=/", NULL };
            execve("/bin/sh", argv, envp);
            /* If exec returns, it failed */
            printf("[init] execve /bin/sh failed, trying /bin/sh.elf...\n");
            execve("/bin/sh.elf", argv, envp);
            printf("[init] execve failed\n");
            exit(1);
        }

        /* Parent — wait for children */
        int status = 0;
        waitpid(pid, &status, 0);
        printf("[init] Shell exited (status %d), respawning...\n", status);
    }

    /* Fallback — just loop */
    printf("[init] All shells failed, halting\n");
    for (;;) { /* pause */ }
    return 0;
}
