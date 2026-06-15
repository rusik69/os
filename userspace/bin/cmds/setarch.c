/* setarch.c — print/set architecture (personality) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc>1){
        if(strcmp(argv[1],"--list")==0){
            printf("x86_64\nlinux32\n");
            return 0;
        }
        /* Run command with given arch */
        if(argc>2){
            /* We don't have personality() syscall wrapper, but we can exec the command */
            printf("setarch: running command with arch '%s'\n",argv[1]);
            execve(argv[2],argv+2,0);
            printf("setarch: cannot exec %s\n",argv[2]);
            return 1;
        }
        printf("Usage: setarch <arch> <command> [args...]\n");
        return 1;
    }

    /* Show current architecture from uname */
    struct utsname buf;
    if(uname(&buf)>=0){
        printf("%s\n",buf.machine);
        return 0;
    }
    printf("x86_64\n");
    return 0;
}
