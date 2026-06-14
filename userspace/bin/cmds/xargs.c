/* xargs.c — read args from stdin, execute command */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_ARGS 256
#define BUF_SIZE 65536

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: xargs <command> [initial-args...]\n"); return 1; }
    const char *cmd = argv[1];
    char *init_args[MAX_ARGS];
    int n_init = 0;
    for (int i = 2; i < argc && n_init < MAX_ARGS - 1; i++)
        init_args[n_init++] = argv[i];
    init_args[n_init] = 0;
    char buf[BUF_SIZE];
    int total = 0;
    int n;
    while ((n = read(STDIN_FILENO, buf + total, BUF_SIZE - total - 1)) > 0)
        total += n;
    if (total == 0 && n_init == 0) return 0;
    buf[total] = '\0';
    char *tokens[MAX_ARGS];
    int nt = 0;
    int in_word = 0;
    for (int i = 0; i <= total; i++) {
        char c = buf[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\0') {
            if (in_word) { buf[i] = '\0'; in_word = 0; }
        } else {
            if (!in_word) {
                if (nt >= MAX_ARGS) break;
                tokens[nt++] = buf + i;
                in_word = 1;
            }
        }
    }
    /* Build argv for each invocation */
    char *exec_args[MAX_ARGS];
    int exec_pos = 0;
    for (int i = 0; i < n_init; i++)
        exec_args[exec_pos++] = init_args[i];
    for (int ti = 0; ti <= nt; ti++) {
        if (ti == nt || exec_pos >= MAX_ARGS - 1) {
            /* Execute */
            exec_args[exec_pos] = 0;
            if (exec_pos > n_init) {
                int ret = posix_spawn(cmd, exec_args, 0);
                if (ret < 0) { printf("xargs: exec failed\n"); return 1; }
            }
            exec_pos = n_init;
        }
        if (ti < nt)
            exec_args[exec_pos++] = tokens[ti];
    }
    return 0;
}
