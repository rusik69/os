/* tetris.c — Tetris is a kernel-level service */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("Tetris is available in the kernel shell as 'tetris' command.\n");
    printf("Use the kernel shell for interactive gameplay.\n");
    return 0;
}
