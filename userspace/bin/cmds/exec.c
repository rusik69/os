/* exec.c — execute command (replaces current process) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: exec <path> [args...]\n");
        return 1;
    }

    /* Pass current environment */
    execve(argv[1],argv+1,0);

    /* If execve returns, there was an error */
    printf("exec: cannot exec %s\n",argv[1]);
    return 1;
}
