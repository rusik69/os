/* script.c — record terminal session using pipe, fork, execve */
#include "unistd.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    const char *fname = "typescript";
    if (argc > 1)
        fname = argv[1];

    /* Open output file */
    int outfd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        write(2, "script: cannot create ", 22);
        write(2, fname, strlen(fname));
        write(2, "\n", 1);
        return 1;
    }

    /* Create pipes for child communication.
     * p2c: parent writes, child reads (stdin)
     * c2p: child writes (stdout), parent reads */
    int p2c[2], c2p[2];
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        write(2, "script: pipe failed\n", 20);
        close(outfd);
        return 1;
    }

    int pid = fork();
    if (pid < 0) {
        write(2, "script: fork failed\n", 20);
        close(outfd);
        return 1;
    }

    if (pid == 0) {
        /* Child process — will run /bin/sh */
        close(p2c[1]);   /* close write end of parent->child pipe */
        close(c2p[0]);   /* close read end of child->parent pipe */

        /* Redirect stdin from pipe */
        dup2(p2c[0], 0);
        /* Redirect stdout and stderr to pipe */
        dup2(c2p[1], 1);
        dup2(c2p[1], 2);

        close(p2c[0]);
        close(c2p[1]);

        /* Start shell */
        char *sh_argv[] = {(char *)"/bin/sh", 0};
        execve("/bin/sh", sh_argv, 0);

        /* If exec fails */
        write(2, "script: exec /bin/sh failed\n", 28);
        exit(1);
    }

    /* Parent process */
    close(p2c[0]);   /* close read end of p2c */
    close(c2p[1]);   /* close write end of c2p */

    /* Write header to log */
    const char *start_msg = "Script started on ";
    write(outfd, start_msg, strlen(start_msg));
    write(outfd, "\n", 1);

    /* Fork a helper to handle stdin forwarding.
     * Helper reads from stdin (user input), sends to child via pipe,
     * and also logs input to the typescript file. */
    int helper_pid = fork();
    if (helper_pid == 0) {
        /* Helper process: forward stdin to child and log */
        close(c2p[0]); /* not needed */
        char ibuf[1024];
        int r;
        while ((r = read(0, ibuf, sizeof(ibuf))) > 0) {
            write(p2c[1], ibuf, r);
            write(outfd, ibuf, r);
        }
        /* Close pipe to signal EOF to child */
        close(p2c[1]);
        close(outfd);
        exit(0);
    }

    /* Main reader: read child output, write to stdout and log */
    close(p2c[1]); /* parent doesn't write to p2c, helper does */
    char obuf[4096];
    int r;
    while ((r = read(c2p[0], obuf, sizeof(obuf))) > 0) {
        write(1, obuf, r);
        write(outfd, obuf, r);
    }
    close(c2p[0]);

    /* Wait for helper and shell child */
    int status;
    waitpid(helper_pid, &status, 0);
    waitpid(pid, &status, 0);

    /* Write end marker */
    const char *end_msg = "\nScript done\n";
    write(outfd, end_msg, strlen(end_msg));
    close(outfd);

    write(1, "\nScript done, output written to ", 32);
    write(1, fname, strlen(fname));
    write(1, "\n", 1);

    return 0;
}
