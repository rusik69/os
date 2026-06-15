/* doom.c — DOOM is a kernel-level service */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("DOOM is available in the kernel shell.\n");
    printf("Use the kernel shell for DOOM gameplay.\n");
    return 0;
}
