/* exec.c — execute command */
#include "unistd.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: exec <path> [args...]\n");return 1;}
    execve(argv[1],argv+1,0);
    printf("exec: cannot exec %s\n",argv[1]);
    return 1;
}
