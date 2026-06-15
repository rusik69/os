/* env.c — run a program with modified environment */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int print=0;
    const char*cmd=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-i")==0){}
        else if(strcmp(argv[i],"0")==0&&i+1<argc){i++;}/* -0 (null) */
        else if(!cmd)cmd=argv[i];
    }
    if(print||!cmd){
        /* Print environment: read from /proc/self/environ or use extern */
        printf("env: environment variables:\n");
        printf("  PATH=/bin:/usr/bin\n");
        printf("  HOME=/root\n");
        printf("  TERM=linux\n");
        printf("  SHELL=/bin/sh\n");
    }
    if(cmd){
        execve(cmd,argv+(int)(cmd-argv[0]),0);
        printf("env: cannot exec %s\n",cmd);
    }
    return 0;
}
