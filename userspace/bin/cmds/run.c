/* run.c — run a command with fork+exec+wait */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("usage: run <command> [args...]\n");
        return 1;
    }

    int pid=fork();
    if(pid<0){
        printf("run: fork failed\n");
        return 1;
    }

    if(pid==0){
        /* Child: exec the command */
        execve(argv[1],argv+1,0);
        printf("run: cannot execute '%s'\n",argv[1]);
        exit(1);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid,&status,0);

    if(status!=0) return 1;
    return 0;
}
