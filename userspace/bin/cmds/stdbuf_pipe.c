/* stdbuf_pipe.c — like stdbuf but for pipe data flow */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static void usage(void) {
    printf("usage: stdbuf_pipe -i <size> -o <size> -e <size> <command [args...]>\n");
    printf("Like stdbuf but controls buffering for piped I/O.\n");
    printf("  -i <size>   stdin buffer size (0 = unbuffered, line = line-buffered)\n");
    printf("  -o <size>   stdout buffer size\n");
    printf("  -e <size>   stderr buffer size\n");
}

int main(int argc, char *argv[]) {
    unsigned long size_i = 0, size_o = 0, size_e = 0;
    int set_i = 0, set_o = 0, set_e = 0;

    if (argc < 2) {
        usage();
        return 1;
    }

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            size_i = (unsigned long)atoi(argv[i + 1]);
            set_i = 1;
            i += 2;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            size_o = (unsigned long)atoi(argv[i + 1]);
            set_o = 1;
            i += 2;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            size_e = (unsigned long)atoi(argv[i + 1]);
            set_e = 1;
            i += 2;
        } else {
            printf("stdbuf_pipe: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (i >= argc) {
        printf("stdbuf_pipe: missing command\n");
        return 1;
    }

    /* Set buffer sizes for current process before forking.
     * In a full implementation, we'd use setvbuf(). Since our libc doesn't
     * have it, we create pipes to control buffering behavior. */

    /* For pipe mode, create a buffer pipe for stdin if requested */
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (set_i && size_i > 0) {
        /* Create a buffer pipe for stdin */
        if (pipe(stdin_pipe) < 0) {
            printf("stdbuf_pipe: pipe failed\n");
            return 1;
        }
    }

    if (set_o && size_o > 0) {
        if (pipe(stdout_pipe) < 0) {
            printf("stdbuf_pipe: pipe failed\n");
            return 1;
        }
    }

    if (set_e && size_e > 0) {
        if (pipe(stderr_pipe) < 0) {
            printf("stdbuf_pipe: pipe failed\n");
            return 1;
        }
    }

    int pid = fork();
    if (pid < 0) {
        printf("stdbuf_pipe: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: wire up pipe buffers */
        if (stdin_pipe[0] >= 0) {
            close(stdin_pipe[1]);
            dup2(stdin_pipe[0], 0);
            close(stdin_pipe[0]);
        }
        if (stdout_pipe[1] >= 0) {
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], 1);
            close(stdout_pipe[1]);
        }
        if (stderr_pipe[1] >= 0) {
            close(stderr_pipe[0]);
            dup2(stderr_pipe[1], 2);
            close(stderr_pipe[1]);
        }

        execve(argv[i], argv + i, NULL);
        printf("stdbuf_pipe: exec '%s' failed\n", argv[i]);
        return 1;
    }

    /* Parent: close unused ends, shuttle data through pipes if needed */
    if (stdin_pipe[0] >= 0) {
        close(stdin_pipe[0]);
        /* Shuttle stdin through the buffered pipe */
        char buf[4096];
        int n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(stdin_pipe[1], buf, n);
        close(stdin_pipe[1]);
    }
    /* Shutdown write ends so child sees EOF */
    if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    if (status != 0)
        return status;
    return 0;
}
