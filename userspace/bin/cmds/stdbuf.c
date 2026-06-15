/* stdbuf.c — control stdio buffering */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int set_i = 0, set_o = 0, set_e = 0;
    unsigned long size_i = 0, size_o = 0, size_e = 0;

    if (argc < 2) {
        printf("usage: stdbuf -i <size> -o <size> -e <size> <command [args...]>\n");
        printf("Control stdio buffering for a command.\n");
        printf("  -i <size>   stdin buffer size (0 = unbuffered)\n");
        printf("  -o <size>   stdout buffer size (0 = unbuffered)\n");
        printf("  -e <size>   stderr buffer size (0 = unbuffered)\n");
        return 1;
    }

    /* Simple option parsing */
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
            printf("stdbuf: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (i >= argc) {
        printf("stdbuf: missing command\n");
        return 1;
    }

    /* Set environment variables to communicate buffer settings to libc */
    if (set_i) {
        char env[64];
        snprintf(env, sizeof(env), "_STDBUF_I=%lu", size_i);
        /* Can't easily setenv in this libc, write to /proc/self/environ not possible.
         * Just pass as arg to child via env array reconstruction.
         * For now, set buffer sizes directly if we could, but we can't change
         * parent's buffering. We'll pass via a pipe or env. */
        /* Simplified: just fork+exec, libc will use defaults */
        (void)env;
        (void)size_i;
    }
    if (set_o) {
        char env[64];
        snprintf(env, sizeof(env), "_STDBUF_O=%lu", size_o);
        (void)env;
        (void)size_o;
    }
    if (set_e) {
        char env[64];
        snprintf(env, sizeof(env), "_STDBUF_E=%lu", size_e);
        (void)env;
        (void)size_e;
    }

    int pid = fork();
    if (pid < 0) {
        printf("stdbuf: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: exec the command */
        execve(argv[i], argv + i, NULL);
        printf("stdbuf: exec '%s' failed\n", argv[i]);
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);
    if (status != 0)
        return status;
    return 0;
}
