/* Init process — PID 1 for userspace.
 *
 * Opens /dev/console, spawns /bin/sh, then waits for children.
 * This is the first userspace process started by the kernel.
 *
 * Signal handling:
 *   SIGTERM — initiates shutdown sequence: kills all children,
 *             syncs filesystems, then calls reboot().
 *   SIGINT  — forwarded to the child process (break/interrupt).
 *
 * SIGKILL cannot be caught or ignored.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* PID of the current child (getty/shell) — used by signal handlers */
static volatile int g_child_pid = 0;

/* Flag set when a shutdown signal has been received */
static volatile int g_shutting_down = 0;

/* Forward a signal to the child process (if any). */
static void forward_signal_to_child(int signum) {
    int pid = g_child_pid;
    if (pid > 0) {
        kill(pid, signum);
    }
}

/* Shutdown handler — triggered by SIGTERM.
 * Sets the shutdown flag and forwards the signal to the child process.
 * The main loop will detect the flag and perform the full shutdown
 * (sync filesystems, call reboot()) instead of respawning the child. */
static void shutdown_handler(int signum) {
    if (g_shutting_down)
        return; /* Already shutting down — prevent re-entry */
    g_shutting_down = 1;
    printf("[init] Shutdown signal (%d) received, terminating children...\n", signum);

    /* Forward SIGTERM to the child process (getty/shell) */
    int pid = g_child_pid;
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

/* Reap any zombie children (including orphaned grandchildren that
 * have been reparented to init).  This prevents accumulation of
 * zombies that no other process is waiting for.
 * Returns the number of children reaped, or 0 if none. */
static int reap_children(void) {
    int reaped = 0;
    int status;
    while (1) {
        int pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        printf("[init] Reaped child %d (status %d)\n", pid, status);
        reaped++;
    }
    return reaped;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("[init] PID %d: Starting init process...\n", getpid());

    /* Install signal handlers:
     *   SIGTERM — initiate full shutdown sequence
     *   SIGINT  — forward to child as break/interrupt */
    signal(SIGTERM, shutdown_handler);
    signal(SIGINT, forward_signal_to_child);

    /* Open /dev/console for stdin/stdout/stderr */
    int console = open("/dev/console", 0);
    if (console < 0) {
        /* Maybe /dev doesn't exist yet — try stdin/stdout directly */
        printf("[init] Warning: no /dev/console, using raw I/O\n");
    } else {
        /* Map the console fd to stdin/stdout/stderr */
        if (console != 0) {
            dup2(console, 0);
            dup2(console, 1);
            dup2(console, 2);
            close(console);
        }
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
            /* Child — exec getty on console */
            char *const argv[] = {"/bin/getty", "/dev/console", NULL};
            char *const envp[] = {"PATH=/bin", "HOME=/", NULL};
            execve("/bin/getty", argv, envp);
            /* If exec returns, it failed — fallback to direct shell */
            printf("[init] execve /bin/getty failed, trying /bin/sh...\n");
            {
                char *const sh_argv[] = {"/bin/sh", NULL};
                execve("/bin/sh", sh_argv, envp);
            }
            printf("[init] execve /bin/sh also failed\n");
            exit(1);
        }

        /* Parent — save child PID for signal forwarding, then reap
         * any zombie children before blocking on the getty */
        g_child_pid = pid;
        reap_children();

        /* Wait for the getty (and its shell) to exit */
        int status = 0;
        waitpid(pid, &status, 0);

        /* Reap again after getty exits (collect any remaining orphans) */
        reap_children();

        /* If shutdown was requested, perform the full shutdown sequence
         * instead of respawning the child */
        if (g_shutting_down) {
            printf("[init] Shutdown in progress — finalizing...\n");

            /* Sync filesystems to flush all pending writes */
            printf("[init] Syncing filesystems...\n");
            sync();
            sync();

            /* Halt/reboot via the reboot() syscall */
            printf("[init] Calling reboot()...\n");
            reboot();

            /* reboot() should not return */
            printf("[init] reboot() returned unexpectedly, halting\n");
            for (;;) { /* pause */ }
        }

        printf("[init] Getty exited (status %d), respawning...\n", status);
    }

    /* Fallback — just loop */
    printf("[init] All shells failed, halting\n");
    for (;;) { /* pause */ }
    return 0;
}
