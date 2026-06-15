/* setsid.c — run command in new session */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* SYS_SETSID syscall number */
#define SYS_SETSID 112

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: setsid <command> [args...]\n");
        return 1;
    }

    int pid=fork();
    if(pid<0){
        printf("setsid: fork failed\n");
        return 1;
    }

    if(pid==0){
        /* Child: create new session */
        long ret;
        __asm__ volatile (
            "syscall"
            : "=a"(ret)
            : "a"((long)SYS_SETSID)
            : "rcx", "r11"
        );

        /* Execute command */
        execve(argv[1],argv+1,0);
        printf("setsid: cannot exec %s\n",argv[1]);
        exit(1);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid,&status,0);

    if(status!=0) return 1;
    return 0;
}
