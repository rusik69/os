/* hd.c — hex dump: just call hexdump with same args */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    /* Re-exec as hexdump */
    char *new_argv[256];
    int j = 0;
    new_argv[j++] = "hexdump";
    for (int i = 1; i < argc && j < 255; i++)
        new_argv[j++] = argv[i];
    new_argv[j] = NULL;
    execve("hexdump", new_argv, NULL);
    /* If exec fails */
    printf("hd: hexdump not found\n");
    return 1;
}
