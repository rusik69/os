/* setsid.c — run command in new session */
#include "unistd.h"
#include "stdio.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: setsid <command> [args...]\n");return 1;}
    int pid=fork();
    if(pid<0){printf("setsid: fork failed\n");return 1;}
    if(pid==0){/* child: new session */
        execve(argv[1],argv+1,0);
        printf("setsid: cannot exec %s\n",argv[1]);return 1;
    }
    /* parent: wait for child */
    int status;waitpid(pid,&status,0);
    return 0;
}
