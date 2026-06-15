/* nice.c — run command with modified priority */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: nice [-n ADJUSTMENT] COMMAND [ARG...]\n");
        return 1;
    }
    int adjustment = 10;
    int cmd_start = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        adjustment = atoi(argv[2]);
        cmd_start = 3;
    }
    if (cmd_start >= argc) {
        printf("nice: missing command\n");
        return 1;
    }
    printf("nice: running '%s' with priority adjustment %d\n",
           argv[cmd_start], adjustment);
    char *new_argv[64];
    int j = 0;
    for (int i = cmd_start; i < argc && j < 63; i++, j++)
        new_argv[j] = argv[i];
    new_argv[j] = 0;
    int pid = posix_spawn(new_argv[0], new_argv, 0);
    if (pid < 0) {
        printf("nice: cannot run '%s'\n", new_argv[0]);
        return 1;
    }
    int status;
    waitpid(pid, &status, 0);
    return status;
}
